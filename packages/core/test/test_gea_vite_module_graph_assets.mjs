import assert from 'node:assert/strict'
import crypto from 'node:crypto'
import fs from 'node:fs'
import path from 'node:path'
import { spawnSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'
import { build } from 'vite'

import { geaAppleNativeModuleAliases, geaModuleGraphPlugins } from '../scripts/gea-vite-module-graph-plugin.mjs'

const packageDir = path.dirname(fileURLToPath(new URL('../package.json', import.meta.url)))
const repoRoot = path.resolve(packageDir, '../..')
// The same gitignored build directory every other test here writes to. It used
// to be a `.scratch/` of its own at the repo root -- a second place for
// throwaway output, invented by this one file.
const scratchRoot = path.join(packageDir, 'test/.build', 'gea-vite-module-graph-' + process.pid)
const processTmpDir = path.join(scratchRoot, 'tmp')

fs.rmSync(scratchRoot, { recursive: true, force: true })
fs.mkdirSync(processTmpDir, { recursive: true })

try {
  await testStaticAssetGraph()
  await testEntryReachableGraph()
  await testAppleNativeAliasGraph()
} finally {
  fs.rmSync(scratchRoot, { recursive: true, force: true })
}

async function testEntryReachableGraph() {
  const appDir = path.join(scratchRoot, 'entry-reachable-app')
  const graphDir = path.join(scratchRoot, 'entry-reachable-graph')
  fs.mkdirSync(appDir, { recursive: true })
  const entryPath = path.join(appDir, 'index.js')
  const directPath = path.join(appDir, 'direct.js')
  const virtualDependencyPath = path.join(appDir, 'virtual-dependency.js')
  fs.writeFileSync(entryPath, "import { direct } from './direct.js'\nimport { virtualValue } from 'virtual:test-runtime'\nexport const result = direct + virtualValue\n")
  fs.writeFileSync(directPath, 'export const direct = 1\n')
  fs.writeFileSync(virtualDependencyPath, 'export const virtualValue = 2\n')

  const virtualRuntimePlugin = {
    name: 'test-virtual-runtime',
    resolveId(id) {
      if (id === 'virtual:test-runtime') return '\0virtual:test-runtime'
    },
    load(id) {
      if (id === '\0virtual:test-runtime') {
        return `export { virtualValue } from ${JSON.stringify(virtualDependencyPath)}\n`
      }
    },
  }

  await build({
    root: appDir,
    configFile: false,
    logLevel: 'silent',
    plugins: [virtualRuntimePlugin, ...geaModuleGraphPlugins({ outDir: graphDir, entryReachableOnly: true })],
    build: {
      lib: { entry: entryPath, formats: ['es'], fileName: () => 'index.js' },
      outDir: path.join(scratchRoot, 'entry-reachable-dist'),
      emptyOutDir: true,
      minify: false,
    },
  })

  const graph = JSON.parse(fs.readFileSync(path.join(graphDir, 'gea-module-graph.json'), 'utf8'))
  const ids = graph.modules.map((module) => module.id)
  assert.ok(ids.includes(fs.realpathSync(entryPath)))
  assert.ok(ids.includes(fs.realpathSync(directPath)), 'normal entry dependency must remain compiler-visible')
  assert.ok(!ids.includes(fs.realpathSync(virtualDependencyPath)), 'hybrid bundle owns dependencies behind virtual modules')
}

async function testStaticAssetGraph() {
  const appDir = path.join(scratchRoot, 'asset-app')
  const soundsDir = path.join(appDir, 'sounds')
  const fontsDir = path.join(appDir, 'fonts')
  const imagesDir = path.join(appDir, 'images')
  fs.mkdirSync(soundsDir, { recursive: true })
  fs.mkdirSync(fontsDir, { recursive: true })
  fs.mkdirSync(imagesDir, { recursive: true })

  const audioPath = path.join(soundsDir, 'coin chime.mp3')
  const fontPath = path.join(fontsDir, 'playfair.woff')
  const audioContents = Buffer.from([0x49, 0x44, 0x33, 0x04, 0x00, 0x00, 0x01])
  // A binary-looking wOFF header guards against feeding assets to the JS parser.
  const fontContents = Buffer.from([0x77, 0x4f, 0x46, 0x46, 0x00, 0x01, 0x00, 0x00, 0xff])
  fs.writeFileSync(audioPath, audioContents)
  fs.writeFileSync(fontPath, fontContents)
  fs.writeFileSync(path.join(imagesDir, 'background.png'), Buffer.from([0x89, 0x50, 0x4e, 0x47]))
  fs.writeFileSync(path.join(appDir, 'style.css'), "body { background-image: url('./images/background.png'); }\n")
  fs.writeFileSync(
    path.join(appDir, 'index.js'),
    [
      "import coinUrl from './sounds/coin chime.mp3'",
      "import fontUrl from './fonts/playfair.woff'",
      "import './style.css'",
      'export { coinUrl, fontUrl }',
      '',
    ].join('\n'),
  )

  const first = await buildAssetGraph(appDir, 'first')
  const second = await buildAssetGraph(appDir, 'second')
  assert.deepEqual(stableGraphProjection(first), stableGraphProjection(second))
  assert.equal(first.assetCount, 2)
  assert.equal(first.assets.length, 2)
  assert.deepEqual(
    first.assets.map((asset) => asset.moduleId),
    [...first.assets.map((asset) => asset.moduleId)].sort(),
  )

  for (const [inputPath, contents] of [
    [audioPath, audioContents],
    [fontPath, fontContents],
  ]) {
    const source = fs.realpathSync(inputPath)
    const expectedSha256 = crypto.createHash('sha256').update(contents).digest('hex')
    const asset = first.assets.find((item) => item.source === source)
    assert.ok(asset, 'module graph should describe ' + source)
    assert.equal(asset.relativeFile, path.relative(appDir, source))
    assert.equal(asset.bytes, contents.length)
    assert.equal(asset.sha256, expectedSha256)
    assert.equal(asset.url, 'gea-asset://sha256/' + expectedSha256 + '/' + encodeURIComponent(path.basename(source)))

    const module = first.modules.find((item) => item.id === asset.moduleId)
    assert.ok(module, 'synthetic module should be present for ' + source)
    assert.equal(module.file, source + '.geaassetmodule.js')
    assert.equal(module.assetSource, source)
    assert.equal(module.assetSha256, expectedSha256)
    assert.equal(module.assetUrl, asset.url)
    assert.equal(fs.readFileSync(path.join(scratchRoot, 'first-graph', module.originalSource), 'utf8').trim(), 'export default ' + JSON.stringify(asset.url))
  }

  const entryPath = fs.realpathSync(path.join(appDir, 'index.js'))
  const entry = first.modules.find((item) => item.file === entryPath)
  assert.ok(entry, 'entry module should be captured')
  const assetImports = entry.imports.filter((item) => /\.(?:mp3|woff)$/.test(item.specifier))
  assert.equal(assetImports.length, 2)
  assert.ok(
    assetImports.every((item) => item.tracked),
    'plain assets should resolve to tracked JS modules',
  )
  assert.ok(assetImports.every((item) => item.resolvedId.endsWith('.geaassetmodule.js')))
  assert.equal(first.assetCount, 2, 'CSS url() assets must retain Vite CSS semantics')

  // Native firmware must recognize the same content-addressed URL that
  // Vite places in imported asset modules, not only the source-relative path.
  const embeddedFont = path.join(appDir, 'lookup.ttf')
  fs.writeFileSync(embeddedFont, fontContents)
  const embeddedCpp = path.join(scratchRoot, 'embedded-assets.cpp')
  const generate = spawnSync(process.execPath, [path.join(packageDir, 'scripts/generate-gea-embedded-assets.mjs'), '--app-dir', appDir, '--out-cpp', embeddedCpp], { encoding: 'utf8' })
  assert.equal(generate.status, 0, generate.stderr)
  const embeddedUrl = 'gea-asset://sha256/' + crypto.createHash('sha256').update(fontContents).digest('hex') + '/lookup.ttf'
  assert.ok(fs.readFileSync(embeddedCpp, 'utf8').includes(JSON.stringify(embeddedUrl)), 'firmware lookup must include the module graph asset identity')

  const compilerCli = path.join(packageDir, 'node_modules', '@geastack', 'compiler', 'dist', 'cli.js')
  const compile = spawnSync(
    process.execPath,
    [
      compilerCli,
      'compile-module-graph',
      path.join(scratchRoot, 'first-graph', 'gea-module-graph.json'),
      '--out-dir',
      path.join(scratchRoot, 'asset-native'),
      '--allow-any',
    ],
    {
      encoding: 'utf8',
      env: {
        ...process.env,
        GEA_THREE_AUDIO_MUTED: '1',
        TMPDIR: processTmpDir,
      },
    },
  )
  assert.equal(compile.status, 0, compile.stderr)
  assert.doesNotMatch(compile.stderr, /undeclared __gea_global_/, 'asset bindings must be compiler-visible modules')
}

async function buildAssetGraph(appDir, name) {
  const graphDir = path.join(scratchRoot, name + '-graph')
  await build({
    root: appDir,
    configFile: false,
    logLevel: 'silent',
    plugins: geaModuleGraphPlugins({ outDir: graphDir }),
    build: {
      lib: {
        entry: path.join(appDir, 'index.js'),
        formats: ['es'],
        fileName: () => 'index.js',
        cssFileName: 'index',
      },
      outDir: path.join(scratchRoot, name + '-dist'),
      emptyOutDir: true,
      minify: false,
    },
  })
  return JSON.parse(fs.readFileSync(path.join(graphDir, 'gea-module-graph.json'), 'utf8'))
}

async function testAppleNativeAliasGraph() {
  const appDir = path.join(scratchRoot, 'alias-app')
  const nodeModulesDir = path.join(appDir, 'node_modules')
  const sharedConfigDir = path.join(scratchRoot, 'shared-vite-config')
  const sharedNodeModulesDir = path.join(sharedConfigDir, 'node_modules')

  writeJson(path.join(appDir, 'package.json'), { private: true, type: 'module' })
  const threeDir = writeMockThreePackage(nodeModulesDir, 'app')
  const browserTroikaDir = writeMockBrowserTroikaPackage(nodeModulesDir)
  const nativeTroikaDir = writeMockNativeTroikaPackage(nodeModulesDir, 'app')

  // The Vite config deliberately lives outside the app and has a complete,
  // conflicting package tree. Native identity must still come from appDir.
  writeJson(path.join(sharedConfigDir, 'package.json'), { private: true, type: 'module' })
  const sharedThreeDir = writeMockThreePackage(sharedNodeModulesDir, 'shared-config')
  const sharedNativeTroikaDir = writeMockNativeTroikaPackage(sharedNodeModulesDir, 'shared-config')

  const entryPath = path.join(appDir, 'index.js')
  fs.writeFileSync(
    entryPath,
    [
      "import { Marker } from 'three/src/Marker.js'",
      "import { Text } from 'troika-three-text'",
      'export const direct = new Marker()',
      'export const text = new Text()',
      '',
    ].join('\n'),
  )
  const viteConfigPath = path.join(sharedConfigDir, 'vite.config.mjs')
  writeViteConfig(viteConfigPath, entryPath)

  const browserGraphDir = path.join(scratchRoot, 'browser-alias-graph')
  await build({
    root: appDir,
    configFile: false,
    logLevel: 'silent',
    plugins: geaModuleGraphPlugins({ outDir: browserGraphDir }),
    build: {
      lib: { entry: entryPath, formats: ['es'], fileName: () => 'index.js' },
      outDir: path.join(scratchRoot, 'browser-alias-dist'),
      emptyOutDir: true,
      minify: false,
    },
  })
  const browserGraph = JSON.parse(fs.readFileSync(path.join(browserGraphDir, 'gea-module-graph.json'), 'utf8'))
  assert.ok(
    browserGraph.modules.some((module) => module.id === path.join(threeDir, 'build', 'three.module.js')),
    'control graph should contain the browser Three bundle',
  )

  assert.throws(
    () => geaAppleNativeModuleAliases({ troikaThreeTextModule: '@geastack/native-webgl-angle/troika-three-text' }),
    /must be an absolute module resolved from the application package boundary/,
  )
  assert.throws(
    () => geaAppleNativeModuleAliases({ threeWebXRManagerModule: '@geastack/native-webgl-angle/nativeWebXRManager' }),
    /must be an absolute module resolved from the application package boundary/,
  )
  assert.throws(
    () => geaAppleNativeModuleAliases({ threeWebGLAnimationModule: '@geastack/native-webgl-angle/nativeWebGLAnimation' }),
    /must be an absolute module resolved from the application package boundary/,
  )
  assert.throws(
    () => geaAppleNativeModuleAliases({ threeUtilsModule: '@geastack/native-webgl-angle/nativeThreeUtils' }),
    /must be an absolute module resolved from the application package boundary/,
  )
  const noTroikaAliases = geaAppleNativeModuleAliases({ threeSrcDir: path.join(threeDir, 'src') })
  assert.equal(noTroikaAliases.length, 1, 'no bare-specifier Troika fallback may be emitted')
  assert.ok(!noTroikaAliases.some((alias) => alias.find.test('troika-three-text')))

  const nativeTroikaModule = path.join(nativeTroikaDir, 'troika-three-text.js')
  const nativeWebXRManagerModule = path.join(nativeTroikaDir, 'nativeWebXRManager.js')
  const nativeWebGLAnimationModule = path.join(nativeTroikaDir, 'nativeWebGLAnimation.js')
  const nativeThreeUtilsModule = path.join(nativeTroikaDir, 'nativeThreeUtils.js')
  const aliases = geaAppleNativeModuleAliases({
    threeSrcDir: path.join(threeDir, 'src'),
    threeUtilsModule: nativeThreeUtilsModule,
    threeWebGLAnimationModule: nativeWebGLAnimationModule,
    threeWebXRManagerModule: nativeWebXRManagerModule,
    troikaThreeTextModule: nativeTroikaModule,
  })
  assert.equal(aliases.length, 5)
  assert.equal('./webgl/WebGLAnimation.js'.replace(aliases[0].find, aliases[0].replacement), nativeWebGLAnimationModule)
  assert.equal(aliases[1].replacement, nativeWebXRManagerModule)
  assert.equal('./webxr/WebXRManager.js'.replace(aliases[1].find, aliases[1].replacement), nativeWebXRManagerModule)
  assert.equal('../../utils.js'.replace(aliases[2].find, aliases[2].replacement), nativeThreeUtilsModule)
  assert.equal(aliases[3].replacement, nativeTroikaModule)
  assert.equal('three/src/Marker.js'.replace(aliases[4].find, aliases[4].replacement), path.join(threeDir, 'src', 'Marker.js'))

  const dummyTool = path.join(scratchRoot, 'existing-tool.mjs')
  fs.writeFileSync(dummyTool, 'export default {}\n')
  const nativeOutDir = path.join(scratchRoot, 'native-alias-pipeline')
  const buildScript = path.join(packageDir, 'scripts', 'build-gea-vite-geatsc.mjs')
  const viteBin = path.join(packageDir, 'node_modules', 'vite', 'bin', 'vite.js')
  const nativeBuild = runNativeModuleGraphBuild({ buildScript, appDir, outDir: nativeOutDir, viteConfigPath, viteBin, dummyTool })
  assert.equal(nativeBuild.status, 0, nativeBuild.stderr)

  const generatedConfig = fs.readFileSync(path.join(nativeOutDir, 'gea-apple-native.vite.config.mjs'), 'utf8')
  assert.match(generatedConfig, /geaAppleNativeModuleAliases/)
  assert.ok(generatedConfig.includes(JSON.stringify(path.join(threeDir, 'src'))))
  assert.ok(generatedConfig.includes(JSON.stringify(nativeTroikaModule)))
  assert.ok(!generatedConfig.includes(sharedThreeDir))
  assert.ok(!generatedConfig.includes(sharedNativeTroikaDir))
  assert.doesNotMatch(generatedConfig, /createRequire/, 'generated config must contain frozen app-resolved identities')
  const nativeGraphPath = path.join(nativeOutDir, 'module-graph', 'gea-module-graph.json')
  const nativeGraph = JSON.parse(fs.readFileSync(nativeGraphPath, 'utf8'))
  const nativeIds = nativeGraph.modules.map((module) => module.id)
  assert.ok(nativeIds.includes(nativeTroikaModule))
  assert.ok(nativeIds.includes(path.join(threeDir, 'src', 'Marker.js')))
  assert.ok(!nativeIds.includes(path.join(browserTroikaDir, 'index.js')))
  assert.ok(!nativeIds.some((id) => id.startsWith(sharedThreeDir + path.sep)))
  assert.ok(!nativeIds.some((id) => id.startsWith(sharedNativeTroikaDir + path.sep)))
  assert.ok(!nativeIds.some((id) => id.includes(path.join('three', 'build') + path.sep)))
  assert.equal(nativeIds.filter((id) => id.endsWith(path.join('three', 'src', 'Marker.js'))).length, 1)

  const entry = nativeGraph.modules.find((module) => module.file === fs.realpathSync(entryPath))
  assert.ok(entry, 'native graph should contain the app entry')
  assert.equal(entry.imports.find((item) => item.specifier === 'troika-three-text')?.resolvedId, nativeTroikaModule)

  const missingNativeAppDir = path.join(scratchRoot, 'missing-native-alias-app')
  const missingNodeModulesDir = path.join(missingNativeAppDir, 'node_modules')
  writeJson(path.join(missingNativeAppDir, 'package.json'), { private: true, type: 'module' })
  writeMockThreePackage(missingNodeModulesDir, 'missing-native-app')
  writeMockBrowserTroikaPackage(missingNodeModulesDir)
  // Node resolution climbs through every ancestor node_modules, so a checkout laid
  // out under a directory that installs @geastack/native-webgl-angle would silently
  // satisfy the lookup. Pin the application boundary: its own scope answers first,
  // and exports nothing, so the native subpath is unresolvable from here wherever
  // the checkout lives.
  const boundaryDir = path.join(missingNodeModulesDir, '@geastack', 'native-webgl-angle')
  fs.mkdirSync(boundaryDir, { recursive: true })
  writeJson(path.join(boundaryDir, 'package.json'), { name: '@geastack/native-webgl-angle', version: '0.0.0', exports: {} })
  const missingEntryPath = path.join(missingNativeAppDir, 'index.js')
  fs.writeFileSync(missingEntryPath, "import { Text } from 'troika-three-text'\nexport const text = new Text()\n")
  const missingViteConfigPath = path.join(sharedConfigDir, 'missing-native.vite.config.mjs')
  writeViteConfig(missingViteConfigPath, missingEntryPath)
  const missingBuild = runNativeModuleGraphBuild({
    buildScript,
    appDir: missingNativeAppDir,
    outDir: path.join(scratchRoot, 'missing-native-alias-pipeline'),
    viteConfigPath: missingViteConfigPath,
    viteBin,
    dummyTool,
  })
  assert.notEqual(missingBuild.status, 0, 'an app with browser Troika but no native package must fail')
  assert.match(missingBuild.stderr, /could not resolve "@geastack\/native-webgl-angle\/troika-three-text" from the same application package boundary/)
  assert.ok(missingBuild.stderr.includes(path.join(missingNativeAppDir, 'package.json')))
}

function writeMockThreePackage(nodeModulesDir, marker) {
  const threeDir = path.join(nodeModulesDir, 'three')
  fs.mkdirSync(path.join(threeDir, 'build'), { recursive: true })
  fs.mkdirSync(path.join(threeDir, 'src'), { recursive: true })
  writeJson(path.join(threeDir, 'package.json'), {
    name: 'three',
    version: '1.0.0-' + marker,
    type: 'module',
    exports: {
      '.': './build/three.module.js',
      './src/*': './src/*',
    },
  })
  fs.writeFileSync(path.join(threeDir, 'src', 'Marker.js'), 'export class Marker { constructor() { this.source = ' + JSON.stringify(marker + '-src') + ' } }\n')
  fs.writeFileSync(path.join(threeDir, 'src', 'Three.js'), "export { Marker } from './Marker.js'\n")
  fs.writeFileSync(
    path.join(threeDir, 'build', 'three.module.js'),
    'export class Marker { constructor() { this.source = ' + JSON.stringify(marker + '-build') + ' } }\n',
  )
  return threeDir
}

function writeMockBrowserTroikaPackage(nodeModulesDir) {
  const browserTroikaDir = path.join(nodeModulesDir, 'troika-three-text')
  fs.mkdirSync(browserTroikaDir, { recursive: true })
  writeJson(path.join(browserTroikaDir, 'package.json'), {
    name: 'troika-three-text',
    version: '1.0.0-test',
    type: 'module',
    exports: './index.js',
  })
  fs.writeFileSync(path.join(browserTroikaDir, 'index.js'), "import { Marker } from 'three'\nexport class Text extends Marker {}\n")
  return browserTroikaDir
}

function writeMockNativeTroikaPackage(nodeModulesDir, marker) {
  const nativeTroikaDir = path.join(nodeModulesDir, '@geastack', 'native-webgl-angle')
  fs.mkdirSync(nativeTroikaDir, { recursive: true })
  writeJson(path.join(nativeTroikaDir, 'package.json'), {
    name: '@geastack/native-webgl-angle',
    version: '1.0.0-' + marker,
    type: 'module',
    exports: {
      './troika-three-text': './troika-three-text.js',
    },
  })
  fs.writeFileSync(
    path.join(nativeTroikaDir, 'troika-three-text.js'),
    "import { Marker } from 'three/src/Marker.js'\nexport class Text extends Marker { constructor() { super(); this.nativeSource = " +
      JSON.stringify(marker) +
      ' } }\n',
  )
  return nativeTroikaDir
}

function writeViteConfig(configPath, entryPath) {
  fs.writeFileSync(
    configPath,
    [
      'export default {',
      '  build: {',
      '    lib: { entry: ' + JSON.stringify(entryPath) + ", formats: ['es'], fileName: () => 'index.js' },",
      '    minify: false,',
      '  },',
      '}',
      '',
    ].join('\n'),
  )
}

// @geastack/apple is NOT resolvable from this repo and never will be: apple
// sits above core and depends on it. The generator used to fall back to a
// guessed `<repo>/packages/geastack-apple`, so this arm ran against a path
// that did not exist and asserted nothing about apple -- it passed vacuously.
// The generator now fails closed, so the location is stated: a real
// @geastack/apple if the caller has one, otherwise a minimal stand-in, which
// is all `--module-graph-only` needs (it never loads apple's dist).
function appleRootForTest() {
  if (process.env.GEA_APPLE_ROOT) return process.env.GEA_APPLE_ROOT
  const stub = path.join(scratchRoot, 'apple-package-stand-in')
  fs.mkdirSync(stub, { recursive: true })
  fs.writeFileSync(path.join(stub, 'package.json'), JSON.stringify({ name: '@geastack/apple', version: '0.0.0' }) + '\n')
  return stub
}

function runNativeModuleGraphBuild({ buildScript, appDir, outDir, viteConfigPath, viteBin, dummyTool }) {
  return spawnSync(
    process.execPath,
    [
      buildScript,
      '--app-dir',
      appDir,
      '--out-dir',
      outDir,
      '--entry',
      'index.js',
      '--vite-config',
      viteConfigPath,
      '--vite-bin',
      viteBin,
      '--geatsc-bin',
      dummyTool,
      '--geatsc-gea-plugin',
      dummyTool,
      '--geatsc-apple-native-plugin',
      dummyTool,
      '--apple-native',
      '--module-graph-only',
    ],
    {
      encoding: 'utf8',
      env: {
        ...process.env,
        GEA_THREE_AUDIO_MUTED: '1',
        GEA_APPLE_ROOT: appleRootForTest(),
        TMPDIR: processTmpDir,
      },
    },
  )
}

function stableGraphProjection(graph) {
  const { generatedAt: _generatedAt, ...stable } = graph
  return stable
}

function writeJson(file, value) {
  fs.mkdirSync(path.dirname(file), { recursive: true })
  fs.writeFileSync(file, JSON.stringify(value, null, 2) + '\n')
}
