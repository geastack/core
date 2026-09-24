#!/usr/bin/env node
import fs from 'node:fs'
import path from 'node:path'
import process from 'node:process'
import { spawnSync } from 'node:child_process'
import { createRequire } from 'node:module'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { inlineJsonImports, transformGeaEmbeddedCompatSource } from './gea-embedded-compat-transform.mjs'
import { shouldIgnoreCompatStagingDirectory } from './gea-embedded-compat-staging.mjs'
import { dotEnvDefines, inlineProcessEnv } from './dotenv-defines.mjs'
import {
  MODULE_HINT_SCOPE,
  applyBundledTypeHints,
  collectReachableSourceFiles,
  collectSourceFiles,
  discoverLocalImport,
  parseTsx,
  resolveFunctionHintName,
  restoreModuleGraphElAnnotations,
} from './gea-bundle-type-hints.mjs'

const args = process.argv.slice(2)
const requireFromLib = createRequire(new URL('../package.json', import.meta.url))
const coreRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const elementsRoot = path.resolve(coreRoot, '..', 'elements')
const engineRoot = path.resolve(coreRoot, '..', 'engine')

// Where @geastack/apple actually is, rather than where a repository layout
// would put it. The Apple target ships INSIDE that package, so the repo root it
// reports is the package itself; an app that installs it from npm has it in a
// node_modules chain instead, and `<repo>/packages/geastack-apple` describes
// neither. Guessing the path is what made an installed app fail to resolve
// @geastack/apple/AppKit at all.
function resolveApplePackageRoot(appDir = '') {
  // GEA_APPLE_ROOT is for a caller that IS the apple package: its own target
  // scripts build apps against the working copy, not an installed copy.
  const declared = process.env.GEA_APPLE_ROOT || ''
  if (declared && readPackageName(declared) === '@geastack/apple') return fs.realpathSync(declared)
  for (const start of [appDir, coreRoot]) {
    if (!start) continue
    let dir = path.resolve(start)
    for (;;) {
      if (readPackageName(dir) === '@geastack/apple') return fs.realpathSync(dir)
      const candidate = path.join(dir, 'node_modules', '@geastack', 'apple')
      if (fs.existsSync(path.join(candidate, 'package.json'))) return fs.realpathSync(candidate)
      const parent = path.dirname(dir)
      if (parent === dir) break
      dir = parent
    }
  }
  // No guessed fallback. The old one returned `<repo>/packages/geastack-apple`,
  // a layout that existed only in this project's own working copy, so an
  // installed app was handed a path that was not there and failed later,
  // somewhere else, with a message about the wrong thing.
  fail(`cannot resolve @geastack/apple from ${appDir || coreRoot} — run \`npm install\` there, or set GEA_APPLE_ROOT`)
}

function readPackageName(dir) {
  try {
    return JSON.parse(fs.readFileSync(path.join(dir, 'package.json'), 'utf8')).name
  } catch {
    return ''
  }
}
// The module identity a build pipeline must resolve `@geastack/core` to. The
// package answers "." with two files (`types: ./index.d.ts` for an editor,
// `default: ./runtime.ts` for anything that loads it), and everything that
// compiles the framework from source needs the second one -- see the comment
// at the top of runtime.ts. Stated to Vite as an alias and to TypeScript as a
// `paths` entry, so both consumers of the staged tree agree.
const runtimePath = path.join(coreRoot, 'runtime.ts')
const tryResolve = (m) => { try { return requireFromLib.resolve(m) } catch { return '' } }
const geaPluginResolved = fileURLToPath(import.meta.resolve('@geajs/vite-plugin'))
const { parse } = requireFromLib('@babel/parser')
const traverseModule = requireFromLib('@babel/traverse')
const t = requireFromLib('@babel/types')
const traverse = traverseModule.default || traverseModule
const useStaticCssRules = !['0', 'false', 'no'].includes(String(process.env.GEA_STATIC_CSS_RULES ?? '1').toLowerCase())
const useStaticCssTape = !['0', 'false', 'no'].includes(String(process.env.GEA_STATIC_CSS_TAPE ?? '1').toLowerCase())
const staticCssTapeMinChunk = Math.max(1, Number.parseInt(process.env.GEA_STATIC_CSS_TAPE_MIN_CHUNK ?? '5', 10) || 1)

function readOption(name) {
  const index = args.indexOf(name)
  if (index === -1) return undefined
  return args[index + 1]
}

function hasFlag(name) {
  return args.includes(name)
}

function readAllOptions(name) {
  const values = []
  for (let i = 0; i < args.length; i++) {
    if (args[i] === name) values.push(args[i + 1])
  }
  return values.filter(Boolean)
}

function fail(message) {
  process.stderr.write(`${message}\n`)
  process.exit(1)
}

function acquireOutputPipelineLock(outDir) {
  const lockDir = path.join(path.dirname(outDir), `.${path.basename(outDir)}.gea-pipeline.lock`)
  const pidFile = path.join(lockDir, 'pid')
  const tryAcquire = () => {
    try {
      fs.mkdirSync(lockDir)
      fs.writeFileSync(pidFile, `${process.pid}\n`)
      return true
    } catch (error) {
      if (error?.code !== 'EEXIST') throw error
      return false
    }
  }

  if (!tryAcquire()) {
    const ownerText = fs.existsSync(pidFile) ? fs.readFileSync(pidFile, 'utf8').trim() : ''
    const owner = /^[0-9]+$/.test(ownerText) ? Number(ownerText) : null
    let ownerIsAlive = false
    if (owner !== null) {
      try {
        process.kill(owner, 0)
        ownerIsAlive = true
      } catch {}
    }
    if (!ownerIsAlive) {
      fs.rmSync(lockDir, { recursive: true, force: true })
      if (!tryAcquire()) fail(`generation output became busy while recovering a stale lock: ${outDir}`)
    } else {
      fail(`generation output is already owned by pid ${owner}: ${outDir}`)
    }
  }

  let released = false
  const release = () => {
    if (released) return
    released = true
    const ownerText = fs.existsSync(pidFile) ? fs.readFileSync(pidFile, 'utf8').trim() : ''
    if (ownerText === String(process.pid)) fs.rmSync(lockDir, { recursive: true, force: true })
  }
  process.on('exit', release)
  for (const signal of ['SIGINT', 'SIGTERM', 'SIGHUP']) {
    process.once(signal, () => {
      release()
      process.kill(process.pid, signal)
    })
  }
}

function writeFileIfChanged(filePath, contents) {
  if (fs.existsSync(filePath) && fs.readFileSync(filePath, 'utf8') === contents) return
  fs.mkdirSync(path.dirname(filePath), { recursive: true })
  fs.writeFileSync(filePath, contents)
}

function run(command, commandArgs, options = {}) {
  const result = spawnSync(command, commandArgs, {
    stdio: 'inherit',
    ...options,
  })
  if (result.status !== 0) {
    const rendered = [command, ...commandArgs].join(' ')
    fail(`command failed (${result.status ?? result.signal}): ${rendered}`)
  }
}

function findFirstJsFile(dir) {
  const stack = [dir]
  const files = []
  while (stack.length > 0) {
    const current = stack.pop()
    if (!current || !fs.existsSync(current)) continue
    for (const entry of fs.readdirSync(current, { withFileTypes: true })) {
      const full = path.join(current, entry.name)
      if (entry.isDirectory()) stack.push(full)
      else if (entry.isFile() && entry.name.endsWith('.js')) files.push(full)
    }
  }
  files.sort()
  return files[0]
}

function copyCssUrlAssets(srcCss, dstCss) {
  const css = fs.readFileSync(srcCss, 'utf8')
  const srcDir = path.dirname(srcCss)
  const dstDir = path.dirname(dstCss)
  const urlRe = /url\(\s*(?:"([^"]+)"|'([^']+)'|([^)"']+))\s*\)/gi
  let match
  while ((match = urlRe.exec(css)) !== null) {
    const raw = (match[1] ?? match[2] ?? match[3] ?? '').trim()
    if (!raw || raw.startsWith('#') || raw.startsWith('/') || /^[a-z][a-z0-9+.-]*:/i.test(raw)) continue
    const clean = raw.split(/[?#]/, 1)[0]
    if (!clean) continue
    const srcAsset = path.resolve(srcDir, clean)
    if (!fs.existsSync(srcAsset) || !fs.statSync(srcAsset).isFile()) continue
    const dstAsset = path.resolve(dstDir, clean)
    if (dstAsset === srcAsset) continue
    fs.mkdirSync(path.dirname(dstAsset), { recursive: true })
    fs.copyFileSync(srcAsset, dstAsset)
  }
}

// Stages one source tree into the compat dir; the caller clears the compat
// dir first (an app plus its escaped shared trees stage as multiple calls).
function copyCompatSourceTree(srcDir, dstDir, envDefines = {}) {
  fs.mkdirSync(dstDir, { recursive: true })
  const transformable = new Set(['.js', '.jsx', '.ts', '.tsx'])
  const dstRoot = path.resolve(dstDir)
  const stack = ['.']
  while (stack.length > 0) {
    const rel = stack.pop()
    const currentSrc = path.join(srcDir, rel)
    const currentDst = path.join(dstDir, rel)
    for (const entry of fs.readdirSync(currentSrc, { withFileTypes: true })) {
      if (entry.isDirectory()) {
        if (shouldIgnoreCompatStagingDirectory(entry.name)) continue
        const childRel = path.join(rel, entry.name)
        const childSrc = path.resolve(srcDir, childRel)
        const relativeToDst = path.relative(dstRoot, childSrc)
        if (relativeToDst === '' || (!relativeToDst.startsWith('..') && !path.isAbsolute(relativeToDst))) continue
        fs.mkdirSync(path.join(dstDir, childRel), { recursive: true })
        stack.push(childRel)
        continue
      }
      if (!entry.isFile()) continue
      const childRel = path.join(rel, entry.name)
      const src = path.join(srcDir, childRel)
      const dst = path.join(dstDir, childRel)
      fs.mkdirSync(path.dirname(dst), { recursive: true })
      if (transformable.has(path.extname(entry.name))) {
        const code = fs.readFileSync(src, 'utf8')
        // JSON-import folding first, so the compat transform and the
        // vite-plugin-gea IR extractor both see the data as inline literals
        // (the fully native store/record lowering path). Then bake in .env vars
        // (process.env.<KEY> -> literal) so the module-graph snapshots geatsc
        // compiles carry the value — Vite's `define` only rewrites the bundle.
        fs.writeFileSync(dst, inlineProcessEnv(transformGeaEmbeddedCompatSource(inlineJsonImports(code, src), src), envDefines))
      } else {
        fs.copyFileSync(src, dst)
        if (path.extname(entry.name) === '.css') copyCssUrlAssets(src, dst)
      }
    }
  }
}

// The workspace `node_modules` the app's bare specifiers actually resolve
// through, mirrored beside the staged copy of the app.
//
// Staging copies SOURCES only, so `@geastack/core` -- which resolves in the
// real tree through the hoisted `examples/node_modules/@geastack/core`
// workspace symlink -- resolves through nothing at all in the staged tree. The
// module graph hides that for every module it TRACKS, because it hands geatsc
// each tracked edge's resolved id; a module the graph shook out but the
// typechecker still loads (a barrel's `export { X } from './raster/pipeline'`
// whose exports the app never uses) falls through to ordinary node resolution
// and finds nothing. `examples/apps/gea3d-cube` compiled to "Cannot find module
// '@geastack/core'" for exactly two such files.
//
// EVERY ancestor level is mirrored, not just the nearest: node resolution walks
// up through all of them, and in a workspace the app's own `node_modules` holds
// its direct deps while the hoisted one a few levels up holds the framework.
// Linking only the first found reproduces the failure with a link in place.
function linkCompatNodeModules(appDir, stagedAppDir, compatSrcDir) {
  const stagingRoot = path.resolve(compatSrcDir)
  let real = path.resolve(appDir)
  let staged = path.resolve(stagedAppDir)
  for (;;) {
    const source = path.join(real, 'node_modules')
    if (fs.existsSync(source)) {
      const link = path.join(staged, 'node_modules')
      if (!fs.existsSync(link)) {
        fs.mkdirSync(staged, { recursive: true })
        try {
          fs.symlinkSync(source, link, 'dir')
        } catch {
          // A filesystem without symlinks is not a reason to fail the build:
          // every module the graph tracked still resolves through its stated id.
        }
      }
    }
    if (staged === stagingRoot) return
    const nextReal = path.dirname(real)
    const nextStaged = path.dirname(staged)
    if (nextReal === real || nextStaged === staged) return
    real = nextReal
    staged = nextStaged
  }
}

// Directories OUTSIDE the app dir reached through relative imports (the
// examples/shared component trees). Outermost-only: a nested hit is covered by
// staging its containing directory wholesale (which also brings the css/asset
// files discoverLocalImport skips).
function escapedCompatImportDirs(appDir, entry) {
  const escaped = collectReachableSourceFiles(appDir, entry).filter((file) => path.relative(appDir, file).startsWith('..'))
  const dirs = [...new Set(escaped.map((file) => path.dirname(file)))].sort()
  return dirs.filter((dir, index) => !dirs.slice(0, index).some((outer) => outer !== dir && !path.relative(outer, dir).startsWith('..')))
}

function commonAncestorDir(dirs) {
  let ancestor = dirs[0]
  for (const dir of dirs.slice(1)) {
    while (path.relative(ancestor, dir).startsWith('..')) ancestor = path.dirname(ancestor)
  }
  return ancestor
}

// The compat transform rewrites core imports to the bare specifier
// `gea-embedded`, because that is the name the generated Vite config aliases to
// `runtime.ts` -- the module identity a source-compiling pipeline needs, rather
// than the `index.d.ts` that `@geastack/core` answers with.
//
// geatsc typechecks this same staged tree, and nothing had ever told TypeScript
// that name. `gea-embedded` resolved to nothing for any app: the `paths` entry
// eleven of them still carry points at `examples/lib/gea-embedded`, a directory
// the repo split removed. One authority renaming a module the other cannot see
// is how `Cannot find module 'gea-embedded'` reaches a program whose Vite build
// succeeded (examples/hid-clicker, through the shared Settings tree).
//
// So state the alias to TypeScript too, against the staged runtime when the
// staging mirrored core into the tree -- the module graph is rooted there, and
// pointing the checker outside it would hand back the second module identity
// this whole rename exists to avoid.
function stateRuntimeAliasToTypeScript(stagedAppDir, compatSrcDir, stagingRoot) {
  const mirrored = path.join(compatSrcDir, path.relative(stagingRoot, runtimePath))
  const target = !path.relative(compatSrcDir, mirrored).startsWith('..') && fs.existsSync(mirrored) ? mirrored : runtimePath
  const configPath = path.join(stagedAppDir, 'tsconfig.json')
  // Six apps under examples/ ship no tsconfig at all (ttf-bench and the ios-*
  // family). Handing the checker no project at all is not neutral: it defaults
  // `lib` to include lib.dom, and `@geastack/core` declares `document`,
  // `window`, `console`, `Image`, `WebSocket` and the rest itself -- so every
  // one of them collides ("Definitions of the following identifiers conflict")
  // and `jsx` defaults to unset, which discards the element tree the plugin
  // reads. What those apps need is not a new policy: it is the one their forty
  // five siblings already state in their own tsconfig, so state it for them.
  let config = {
    compilerOptions: {
      target: 'ES2022',
      module: 'ESNext',
      moduleResolution: 'Bundler',
      lib: ['ES2022'],
      strict: true,
      noEmit: true,
      skipLibCheck: true,
      jsx: 'preserve',
    },
    include: ['**/*.ts', '**/*.tsx', '**/*.d.ts'],
  }
  if (fs.existsSync(configPath)) {
    try {
      config = JSON.parse(fs.readFileSync(configPath, 'utf8'))
    } catch (error) {
      // An app tsconfig this script cannot read is the app's to fix, not a
      // reason to fail a build that would otherwise resolve every other module.
      process.stdout.write(`gea-embedded-compat: leaving ${configPath} alone (${error.message})\n`)
      return
    }
  }
  const options = config.compilerOptions ?? (config.compilerOptions = {})
  options.baseUrl = options.baseUrl ?? '.'
  const base = path.resolve(stagedAppDir, options.baseUrl)
  // Both specifiers, because the Vite config aliases both to the same
  // `runtimePath` and an app may write either. `@geastack/core` is the one that
  // matters for `declare module '@geastack/core' { interface
  // GeaIntrinsicElements { ... } }`: left unmapped it resolves to the package's
  // `index.d.ts` OUTSIDE the staged tree, while `JSX.IntrinsicElements` is wired
  // to the staged `runtime.ts` -- so an app's own tags merge into a copy the JSX
  // namespace never reads, and every use of one is `Property 'x' does not exist
  // on type 'JSX.IntrinsicElements'` (companion/examples/gea-companion's
  // `<symbol>`). That is the two-module-identities trap runtime.ts's own header
  // warns about, reached here through a third copy that staging made.
  const stagedRuntime = [path.relative(base, target)]
  options.paths = { ...(options.paths ?? {}), 'gea-embedded': stagedRuntime, '@geastack/core': stagedRuntime }
  // Staging mirrors `examples/node_modules` into the tree, and `@types/react`
  // lives there. TypeScript auto-includes EVERY `@types/*` package it can reach
  // unless `types` is stated, and React's declares a GLOBAL `JSX` namespace --
  // so it does not merely add tags, it supplies the whole namespace. An app
  // whose own JSX declarations resolve never notices. An app whose do not takes
  // React's instead, and every `class` attribute becomes `Property 'class' does
  // not exist ... Did you mean 'className'?` against `DetailedHTMLProps`.
  //
  // `virtual-list/env.d.ts` is the case: it states its types with
  // `/// <reference types="@geastack/core" />`, and a triple-slash TYPES
  // reference resolves through `typeRoots`/`node_modules` ONLY -- never through
  // the `paths` set just above -- while no reachable `node_modules` carries an
  // `@geastack` scope at all. So the reference silently resolves to nothing and
  // React wins by default. These apps declare what they need in their own
  // `.d.ts` and in the staged `runtime.ts`; none of them wants an ambient
  // package. Say so, and keep an app's own `types` if it states one.
  options.types = options.types ?? []
  fs.writeFileSync(configPath, `${JSON.stringify(config, null, 2)}\n`)
}

function resolveAppleNativeAliasModules(appDir) {
  const appPackageBoundary = path.join(appDir, 'package.json')
  const appRequire = createRequire(appPackageBoundary)
  let threeSrcDir = ''
  let threeUtilsModule = ''
  let threeWebGLAnimationModule = ''
  let threeWebXRManagerModule = ''
  let troikaThreeTextModule = ''
  let browserTroikaModule = ''
  try {
    threeSrcDir = path.dirname(fs.realpathSync(appRequire.resolve('three/src/Three.js')))
  } catch {
    // Native apps that do not use Three need no source-identity alias.
  }
  try {
    threeWebGLAnimationModule = fs.realpathSync(appRequire.resolve('@geastack/native-webgl-angle/nativeWebGLAnimation'))
    threeWebXRManagerModule = fs.realpathSync(appRequire.resolve('@geastack/native-webgl-angle/nativeWebXRManager'))
    threeUtilsModule = fs.realpathSync(appRequire.resolve('@geastack/native-webgl-angle/nativeThreeUtils'))
  } catch {
    // A native app that does not use the ANGLE package needs no Three XR replacement.
  }
  try {
    browserTroikaModule = appRequire.resolve('troika-three-text')
  } catch {
    // No browser Troika package means there is no package surface to replace.
  }
  try {
    troikaThreeTextModule = fs.realpathSync(appRequire.resolve('@geastack/native-webgl-angle/troika-three-text'))
  } catch {
    if (browserTroikaModule) {
      fail(
        `Apple-native build resolved "troika-three-text" from ${appPackageBoundary}, but could not resolve ` +
        `"@geastack/native-webgl-angle/troika-three-text" from the same application package boundary. ` +
        'Install the native package in the application; a bare Vite alias cannot be chained safely.'
      )
    }
  }
  return { threeSrcDir, threeUtilsModule, threeWebGLAnimationModule, threeWebXRManagerModule, troikaThreeTextModule }
}

function writeCompatViteConfig({ configPath, compatSrcDir, appDir, entry, viteOutDir, geaIrPath, appleNative, moduleGraphOutDir, envDefines }) {
  const engineComponentsPath = path.join(engineRoot, 'components/index.ts')
  const engineComponentSubpath = path.join(engineRoot, 'components/$1')
  const elementsPath = path.join(elementsRoot, 'components/index.ts')
  const elementSubpath = path.join(elementsRoot, 'components/$1')
  // Only an apple-native build needs the apple package, and only an
  // apple-native build emits the aliases below. Resolving it unconditionally
  // made the (fail-closed) lookup a hard requirement of every ESP32, web and
  // Linux build too.
  const applePackageRoot = appleNative ? resolveApplePackageRoot(appDir) : ''
  const appleRuntimeSubpath = applePackageRoot ? path.join(applePackageRoot, 'runtime/$1.js') : ''
  const applePackagePath = applePackageRoot ? path.join(applePackageRoot, 'dist/index.js') : ''
  const moduleGraphPluginPath = path.join(coreRoot, 'scripts/gea-vite-module-graph-plugin.mjs')
  const { threeSrcDir, threeUtilsModule, threeWebGLAnimationModule, threeWebXRManagerModule, troikaThreeTextModule } = appleNative
    ? resolveAppleNativeAliasModules(appDir)
    : { threeSrcDir: '', threeUtilsModule: '', threeWebGLAnimationModule: '', threeWebXRManagerModule: '', troikaThreeTextModule: '' }
  const q = (value) => JSON.stringify(value)
  // Compile the @geajs/core reactive runtime (Component/Store/compiler-runtime)
  // from its TYPED SOURCE, not the minified dist. The minified build reuses short
  // identifiers across nested scopes, which the C++ symbol collector cannot
  // disambiguate, and — with all types erased — geatsc is forced to lower the
  // whole reactive runtime dynamically (gea_cpp_value, ill-formed auto-vectors,
  // proxy derefs). From source, everything is typed and lowers natively, exactly
  // like three.js/hono/mongodb. The env var still wins when explicitly set;
  // otherwise resolve the package's source entry via its node_modules symlink.
  let compilerRuntimeEntry = process.env.GEA_COMPILER_RUNTIME_SOURCE || ''
  if (!compilerRuntimeEntry) {
    for (const base of [appDir, coreRoot]) {
      try {
        const pkgDir = fs.realpathSync(path.join(base, 'node_modules/@geajs/core'))
        const srcIndex = path.join(pkgDir, 'src/index.ts')
        if (fs.existsSync(srcIndex) && fs.existsSync(path.join(pkgDir, 'src/compiler-runtime.ts'))) {
          compilerRuntimeEntry = srcIndex
          break
        }
      } catch {
        // No @geajs/core source under this base (e.g. published-only install) —
        // fall through; the alias stays empty and the dist runtime is used.
      }
    }
  }
  const compilerRuntimeAlias = compilerRuntimeEntry && fs.existsSync(compilerRuntimeEntry)
    ? `      { find: new RegExp('^@geajs/core$'), replacement: ${q(compilerRuntimeEntry)} },\n`
    : ''
  const appleAliases = appleNative
    ? `      { find: /^@geajs\\/apple\\/(.+)$/, replacement: ${q(appleRuntimeSubpath)} },
      { find: /^@geastack\\/apple\\/(.+)$/, replacement: ${q(appleRuntimeSubpath)} },
      { find: '@geajs/apple', replacement: ${q(applePackagePath)} },
      { find: '@geastack/apple', replacement: ${q(applePackagePath)} },
`
    : ''
  // App .env vars, inlined as process.env.<KEY> string literals. geatsc compiles
  // this bundle (not the browser), so the values are baked in at compile time and
  // no `process` object exists at runtime — process.env.X is already the literal.
  const defineEntries = Object.entries(envDefines || {})
    .map(([key, literal]) => `    ${q(key)}: ${q(literal)},`)
    .join('\n')
  const config = `import { resolve } from 'node:path'
import { geaPlugin } from ${q(geaPluginResolved)}
import { geaAppleNativeModuleAliases, geaModuleGraphPlugins } from ${q(moduleGraphPluginPath)}

const root = ${q(compatSrcDir)}
const geaModuleGraph = geaModuleGraphPlugins({ outDir: ${q(moduleGraphOutDir || '')}, entryReachableOnly: true })
const geaAppleNativeAliases = geaAppleNativeModuleAliases({
  threeSrcDir: ${q(threeSrcDir)},
  threeUtilsModule: ${q(threeUtilsModule)},
  threeWebGLAnimationModule: ${q(threeWebGLAnimationModule)},
  threeWebXRManagerModule: ${q(threeWebXRManagerModule)},
  troikaThreeTextModule: ${q(troikaThreeTextModule)},
})

export default {
  root,
  define: {
${defineEntries}
  },
  plugins: [geaPlugin({ ir: { enabled: true, outFile: ${q(geaIrPath)} } }), ...geaModuleGraph],
  resolve: {
    alias: [
${compilerRuntimeAlias}
      { find: new RegExp('^@geastack/core$'), replacement: ${q(runtimePath)} },
      { find: new RegExp('^@geastack/engine$'), replacement: ${q(engineComponentsPath)} },
      { find: new RegExp('^@geastack/engine/components$'), replacement: ${q(engineComponentsPath)} },
      { find: new RegExp('^@geastack/engine/components/(.+)$'), replacement: ${q(engineComponentSubpath)} },
      { find: new RegExp('^@geastack/elements$'), replacement: ${q(elementsPath)} },
      { find: new RegExp('^@geastack/elements/components$'), replacement: ${q(elementsPath)} },
      { find: new RegExp('^@geastack/elements/components/(.+)$'), replacement: ${q(elementSubpath)} },
      { find: new RegExp('^gea-embedded$'), replacement: ${q(runtimePath)} },
${appleAliases}${appleNative ? '      ...geaAppleNativeAliases,\n' : ''}    ],
  },
  build: {
    lib: {
      entry: resolve(root, ${q(entry)}),
      formats: ['es'],
      fileName: () => 'index.js',
      cssFileName: 'index',
    },
    outDir: ${q(viteOutDir)},
    emptyOutDir: true,
    modulePreload: { polyfill: false },
    minify: false,
    cssCodeSplit: false,
    // Vite 8 / rolldown defaults output.topLevelVar to true, which rewrites
    // top-level \`const\`/\`let\` to \`var\` (a JS-runtime TDZ tweak). That is
    // pointless here — geatsc compiles this bundle to C++, not a browser — and
    // it erases const-ness that geatsc's static analysis relies on (e.g. a
    // const array length / loop bound, which gates dropping typed-array bounds
    // checks). Keep declarations as authored.
    rollupOptions: { output: { topLevelVar: false } },
  },
  // JSON imports that survive source-level folding (see inlineJsonImports)
  // must reach geatsc as an inline object literal, never as the
  // JSON.parse("...") string form vite's stringify:'auto' emits past 10kB —
  // geatsc can only statically type the literal form.
  json: { stringify: false },
}
`
  fs.writeFileSync(configPath, config)
}

function posixPath(value) {
  return value.split(path.sep).join('/')
}

function relativeModuleSpecifier(fromDir, toFile) {
  let specifier = posixPath(path.relative(fromDir, toFile))
  if (!specifier.startsWith('.')) specifier = `./${specifier}`
  return specifier
}

function writeAppendJsCompatEntry({ compatSrcDir, compatEntry, appendFiles }) {
  if (appendFiles.length === 0) return compatEntry
  const entryDirRel = path.dirname(compatEntry)
  const entryDirAbs = path.join(compatSrcDir, entryDirRel)
  const entryAbs = path.join(compatSrcDir, compatEntry)
  const wrapperRel = path.join(entryDirRel, '__gea_appended_entry.tsx')
  const wrapperAbs = path.join(compatSrcDir, wrapperRel)
  const entryImport = relativeModuleSpecifier(path.dirname(wrapperAbs), entryAbs)
  const parts = [`import ${JSON.stringify(entryImport)}\n`]
  for (const appendFile of appendFiles) {
    if (!fs.existsSync(appendFile)) fail(`missing appended JS file: ${appendFile}`)
    parts.push(`\n// appended from ${posixPath(appendFile)}\n${fs.readFileSync(appendFile, 'utf8')}\n`)
  }
  writeFileIfChanged(wrapperAbs, parts.join(''))
  return wrapperRel
}

function stripCssComments(css) {
  return css.replace(/\/\*[\s\S]*?\*\//g, '')
}

function stripAtBlocks(css) {
  return css.replace(/@[^{;]+(?:;|\{[\s\S]*?\})/g, '')
}

function splitTopLevelCss(value, delimiter) {
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

function splitCssWords(value) {
  const out = []
  let i = 0
  while (i < value.length) {
    while (i < value.length && /\s/.test(value[i])) i++
    const start = i
    let depth = 0
    while (i < value.length) {
      const char = value[i]
      if (char === '(') depth++
      else if (char === ')' && depth > 0) depth--
      else if (/\s/.test(char) && depth === 0) break
      i++
    }
    if (i > start) out.push(value.slice(start, i))
  }
  return out
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

function usesGeaEmbeddedCompat(appDir) {
  const transformable = new Set(['.js', '.jsx', '.ts', '.tsx'])
  const stack = [appDir]
  while (stack.length > 0) {
    const current = stack.pop()
    if (!current || !fs.existsSync(current)) continue
    for (const entry of fs.readdirSync(current, { withFileTypes: true })) {
      const full = path.join(current, entry.name)
      if (entry.isDirectory()) {
        if (['node_modules', 'dist', 'build', '.vite'].includes(entry.name)) continue
        stack.push(full)
        continue
      }
      if (!entry.isFile() || !transformable.has(path.extname(entry.name))) continue
      const code = fs.readFileSync(full, 'utf8')
      if (code.includes("'@geastack/core'") || code.includes('"@geastack/core"')) return true
    }
  }
  return false
}

function usesAppleNative(appDir) {
  const transformable = new Set(['.js', '.jsx', '.ts', '.tsx'])
  const stack = [appDir]
  while (stack.length > 0) {
    const current = stack.pop()
    if (!current || !fs.existsSync(current)) continue
    for (const entry of fs.readdirSync(current, { withFileTypes: true })) {
      const full = path.join(current, entry.name)
      if (entry.isDirectory()) {
        if (['node_modules', 'dist', 'build', '.vite'].includes(entry.name)) continue
        stack.push(full)
        continue
      }
      if (!entry.isFile() || !transformable.has(path.extname(entry.name))) continue
      const code = fs.readFileSync(full, 'utf8')
      if (
        code.includes("'@geajs/apple/") ||
        code.includes('"@geajs/apple/') ||
        code.includes("'@geastack/apple/") ||
        code.includes('"@geastack/apple/')
      ) return true
    }
  }
  return false
}

function escapeRegExp(value) {
  return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
}

function typeTextReferencesName(typeText, name) {
  return new RegExp(`\\b${escapeRegExp(name)}\\b`).test(typeText)
}

function resolveTypeImportPath(base) {
  for (const candidate of [
    base,
    `${base}.d.ts`,
    `${base}.ts`,
    `${base}.tsx`,
    path.join(base, 'index.d.ts'),
    path.join(base, 'index.ts'),
    path.join(base, 'index.tsx'),
  ]) {
    if (fs.existsSync(candidate)) return candidate
  }
  return null
}

function typeImportModuleSpecifier(resolvedPath) {
  return resolvedPath
    .replace(/\\/g, '/')
    .replace(/\.d\.ts$/, '')
    .replace(/\.(?:tsx?|jsx?)$/, '')
}

function resolveTypeImportModule(importingFile, source) {
  if (source === '@geastack/core') {
    return typeImportModuleSpecifier(path.join(coreRoot, 'index.d.ts'))
  }
  if (source === '@geastack/engine' || source === '@geastack/engine/components') {
    return typeImportModuleSpecifier(path.join(engineRoot, 'components/index.d.ts'))
  }
  if (source === '@geastack/elements' || source === '@geastack/elements/components') {
    return typeImportModuleSpecifier(path.join(elementsRoot, 'components/index.d.ts'))
  }
  if (source.startsWith('@geastack/core/')) {
    const resolved = resolveTypeImportPath(path.join(coreRoot, source.slice('@geastack/core/'.length)))
    return resolved ? typeImportModuleSpecifier(resolved) : source
  }
  if (source.startsWith('@geastack/engine/components/')) {
    const resolved = resolveTypeImportPath(path.join(engineRoot, 'components', source.slice('@geastack/engine/components/'.length)))
    return resolved ? typeImportModuleSpecifier(resolved) : source
  }
  if (source.startsWith('@geastack/engine/')) {
    const resolved = resolveTypeImportPath(path.join(engineRoot, source.slice('@geastack/engine/'.length)))
    return resolved ? typeImportModuleSpecifier(resolved) : source
  }
  if (source.startsWith('@geastack/elements/components/')) {
    const resolved = resolveTypeImportPath(path.join(elementsRoot, 'components', source.slice('@geastack/elements/components/'.length)))
    return resolved ? typeImportModuleSpecifier(resolved) : source
  }
  if (source.startsWith('@geastack/elements/')) {
    const resolved = resolveTypeImportPath(path.join(elementsRoot, source.slice('@geastack/elements/'.length)))
    return resolved ? typeImportModuleSpecifier(resolved) : source
  }
  if (source.startsWith('.') || source.startsWith('/')) {
    const base = source.startsWith('/') ? source : path.resolve(path.dirname(importingFile), source)
    const resolved = resolveTypeImportPath(base)
    return typeImportModuleSpecifier(resolved ?? base)
  }
  return source
}

function renderTypeImports(imports) {
  const bySource = new Map()
  for (const info of imports) {
    const source = resolveTypeImportModule(info.file, info.source)
    const group = bySource.get(source) ?? []
    group.push(info)
    bySource.set(source, group)
  }
  // Emit each external type as a global `type X = import('mod').Y` ALIAS rather
  // than an `import type { … }` statement. An import statement would make the
  // sidecar a module, so its declarations (and the bundle's `/// <reference>` to
  // it) would no longer be ambient — the restored JSDoc `@type {GeaEmbeddedImage[]}`
  // in the bundle could not resolve the name. An import-type alias is a type
  // expression (not an import statement), so the sidecar stays a script and the
  // alias is globally visible.
  return [...bySource.entries()]
    .sort(([a], [b]) => a.localeCompare(b))
    .flatMap(([source, group]) =>
      group
        .sort((a, b) => a.local.localeCompare(b.local))
        .map((info) => `type ${info.local} = import(${JSON.stringify(source)}).${info.imported}`)
    )
}

// True for `T[]` / `Array<T>` / `ReadonlyArray<T>` where T is a named type
// reference (an interface/type alias). Excludes primitive-element arrays
// (`number[]`, `string[]`) and non-array annotations. Babel TS AST.
function isNamedInterfaceArrayTypeNode(typeNode) {
  if (t.isTSArrayType(typeNode)) {
    return t.isTSTypeReference(typeNode.elementType) && t.isIdentifier(typeNode.elementType.typeName)
  }
  if (t.isTSTypeReference(typeNode) && t.isIdentifier(typeNode.typeName)) {
    const name = typeNode.typeName.name
    if (name !== 'Array' && name !== 'ReadonlyArray') return false
    const args = typeNode.typeParameters?.params
    if (!args || args.length !== 1) return false
    return t.isTSTypeReference(args[0]) && t.isIdentifier(args[0].typeName)
  }
  return false
}

// True for `string[]` / `number[]` / `boolean[]` (and the `Array<…>` form). An
// empty primitive-element array annotation is just as lossy in the bundle as a
// named one — `const keys: string[] = []` infers `any[]` once the pushes live in
// other functions — so it gets the same JSDoc restoration.
function isPrimitiveArrayTypeNode(typeNode) {
  const isPrimitiveElement = (el) =>
    !!el && (t.isTSStringKeyword(el) || t.isTSNumberKeyword(el) || t.isTSBooleanKeyword(el))
  if (t.isTSArrayType(typeNode)) return isPrimitiveElement(typeNode.elementType)
  if (t.isTSTypeReference(typeNode) && t.isIdentifier(typeNode.typeName)) {
    const name = typeNode.typeName.name
    if (name !== 'Array' && name !== 'ReadonlyArray') return false
    const args = typeNode.typeParameters?.params
    return !!args && args.length === 1 && isPrimitiveElement(args[0])
  }
  return false
}

// Array annotations worth restoring as JSDoc: named-element (interface/alias) or
// primitive-element. Both lower to a typed `std::vector<…>` instead of a boxed
// `std::vector<gea_cpp_value>` once the element type is known.
function isHintableArrayTypeNode(typeNode) {
  return isNamedInterfaceArrayTypeNode(typeNode) || isPrimitiveArrayTypeNode(typeNode)
}

// A bare named-type reference (`GeaEmbeddedImage`, `FetchResponse`, an app
// interface) — NOT an array. Bundling erases it to `any` -> `gea_cpp_value`, so a
// field/var/return typed this way loses its static shape (and any `nativeTypes`
// host-handle lowering). Restored as JSDoc, it lowers to the typed struct/native
// type instead. Excludes `Array`/`ReadonlyArray` (handled by the array forms).
function isNamedTypeReferenceNode(typeNode) {
  if (!t.isTSTypeReference(typeNode) || !t.isIdentifier(typeNode.typeName)) return false
  const name = typeNode.typeName.name
  if (name === 'Array' || name === 'ReadonlyArray') return false
  return !typeNode.typeParameters
}

// Field/var/return annotations worth restoring: typed arrays or bare named types.
function isHintableTypeNode(typeNode) {
  return isHintableArrayTypeNode(typeNode) || isNamedTypeReferenceNode(typeNode)
}


function isJsonParseCallNode(expr) {
  return (
    t.isCallExpression(expr) &&
    t.isMemberExpression(expr.callee) &&
    !expr.callee.computed &&
    t.isIdentifier(expr.callee.object, { name: 'JSON' }) &&
    t.isIdentifier(expr.callee.property, { name: 'parse' })
  )
}

// `const x = JSON.parse(...) as T` (or `const x: T = JSON.parse(...)`): the
// bundler erases the `as` cast / annotation, so capture the target type text and
// restore it as a JSDoc `@type`. geatsc then decodes the parse straight into the
// typed struct (one pass, no gea_cpp_value tree) instead of boxing. Gated on a
// JSON.parse initializer so only the exact typed-parse case is promoted.
function jsonParseCastTypeText(declarator, code) {
  if (!declarator.init) return null
  if (t.isTSAsExpression(declarator.init) && isJsonParseCallNode(declarator.init.expression)) {
    const typeNode = declarator.init.typeAnnotation
    if (typeNode?.start != null && typeNode?.end != null) return code.slice(typeNode.start, typeNode.end).trim()
  }
  if (isJsonParseCallNode(declarator.init)) {
    const typeNode = declarator.id.typeAnnotation?.typeAnnotation
    if (typeNode?.start != null && typeNode?.end != null) return code.slice(typeNode.start, typeNode.end).trim()
  }
  return null
}

function collectBundledTypeHints(appDir, entry) {
  const declarations = new Map()
  const typeImports = new Map()
  const returnHints = new Map()
  const paramHints = new Map()
  const classHints = new Map()
  // Class field type annotations (`hours: Forecast[]`). Bundling strips them,
  // so we restore them as JSDoc `@type` and pull the referenced interface into
  // the sidecar. Needed for typed store-array storage that reuses geatsc's
  // `__gea_type_<Name>` struct — the element interface (e.g. `Forecast`) is
  // referenced ONLY by class properties, so without this it never reaches the
  // geatsc checker and the struct is never emitted. Keyed by `Class#prop`.
  const propertyHints = new Map()
  // Function-local typed arrays (`const rows: Row[] = []`) also lose their
  // annotations in the bundle. Restore these as local JSDoc so helper
  // functions that build typed store arrays do not box each row through
  // `std::vector<gea_cpp_value>`. Keyed by `Function#local` or
  // `Class#method#local`.
  const localHints = new Map()
  // Param/return annotations on arrow- and function-EXPRESSIONS that are NOT
  // directly assigned to a named binding — returned closures (`return (x:
  // number) => …`), callback arguments (`arr.map((v: Row) => …)`), and the
  // like. The name-keyed paramHints/returnHints above only reach NAMED
  // functions, methods, and `const f = (…) => …` declarators, so these
  // anonymous callables otherwise lose their param types and every parameter
  // widens to `gea_cpp_value` (the bundle is `.js`, so `isParameterOptional`
  // treats every un-hinted param as optional and the emitter erases the slot —
  // boxing each call's args). Restored as INLINE JSDoc on the callable node.
  // Keyed by `${enclosingHintScope}|${paramNamesJoined}`; the value is the
  // ORDERED list of callables sharing that key, consumed positionally on apply
  // (vite preserves intra-scope expression order), so repeated shapes still map
  // 1:1. Matching on the exact param-name tuple makes a wrong-node application
  // essentially impossible — a mismatch just skips (no regression).
  const inlineCallableHints = new Map()
  const collectInlineCallableHint = (astPath, code) => {
    const node = astPath.node
    const parent = astPath.parent
    // `const f = (x) => …` / `const f = function (x) {}` already restore via the
    // name-keyed VariableDeclarator path; skip to avoid redundant JSDoc.
    if (parent && t.isVariableDeclarator(parent) && parent.init === node) return
    // The structural param-name key only matches across bundling when every
    // parameter is a plain identifier; destructuring/rest params break it.
    if (!node.params.every((p) => t.isIdentifier(p))) return
    const params = []
    for (const param of node.params) {
      const typeNode = param.typeAnnotation?.typeAnnotation
      if (!typeNode || typeNode.start == null || typeNode.end == null) continue
      params.push({ name: param.name, type: code.slice(typeNode.start, typeNode.end).trim(), optional: param.optional === true })
    }
    const returnTypeNode = node.returnType?.typeAnnotation
    const returnType =
      returnTypeNode && returnTypeNode.start != null && returnTypeNode.end != null
        ? code.slice(returnTypeNode.start, returnTypeNode.end).trim()
        : undefined
    if (params.length === 0 && !returnType) return
    const scope = resolveFunctionHintName(astPath) ?? MODULE_HINT_SCOPE
    const key = `${scope}|${node.params.map((p) => p.name).join(',')}`
    const list = inlineCallableHints.get(key) ?? []
    list.push({ params, returnType })
    inlineCallableHints.set(key, list)
  }
  const collectParameterHints = (functionName, params, code) => {
    const hints = []
    for (const param of params) {
      let name = null
      let typeNode = null
      let optional = false
      let rest = false
      if (t.isIdentifier(param)) {
        name = param.name
        typeNode = param.typeAnnotation?.typeAnnotation
        optional = param.optional === true
      } else if (t.isRestElement(param) && t.isIdentifier(param.argument)) {
        name = param.argument.name
        typeNode = param.typeAnnotation?.typeAnnotation ?? param.argument.typeAnnotation?.typeAnnotation
        rest = true
      }
      if (!name || !typeNode) continue
      if (typeNode.start == null || typeNode.end == null) continue
      let typeText = code.slice(typeNode.start, typeNode.end).trim()
      if (rest) {
        const restElementType =
          t.isTSArrayType(typeNode) && typeNode.elementType.start != null && typeNode.elementType.end != null
            ? code.slice(typeNode.elementType.start, typeNode.elementType.end).trim()
            : t.isTSTypeReference(typeNode) &&
                t.isIdentifier(typeNode.typeName) &&
                (typeNode.typeName.name === 'Array' || typeNode.typeName.name === 'ReadonlyArray') &&
                typeNode.typeParameters?.params?.length === 1 &&
                typeNode.typeParameters.params[0].start != null &&
                typeNode.typeParameters.params[0].end != null
              ? code.slice(typeNode.typeParameters.params[0].start, typeNode.typeParameters.params[0].end).trim()
              : typeText
        typeText = `...${restElementType}`
      }
      // Carry the `?` through to the restored JSDoc (`@param {T} [name]`), so
      // the checker types the bundled param `T | undefined` and geatsc keeps a
      // null-capable native slot (std::optional<record>) instead of widening.
      hints.push({ name, type: typeText, optional })
    }
    if (hints.length > 0) paramHints.set(functionName, hints)
  }
  const classNoRuntimeBridgeDirective = (node) =>
    node.leadingComments?.some((comment) => /@gea-no-runtime-bridge\b/.test(comment.value)) === true
  // The gea-embedded runtime's public API (loadImage, loadImageFile, …) is
  // inlined into the bundle by vite, so its signatures vanish exactly like app
  // code's. Walk the runtime sources FIRST so an app function with the same
  // name overrides the runtime hint.
  const hintSourceFiles = [
    ...collectSourceFiles(path.join(coreRoot, 'runtime')),
    ...collectReachableSourceFiles(appDir, entry),
  ]
  for (const file of hintSourceFiles) {
    const code = fs.readFileSync(file, 'utf8')
    const ast = parseTsx(code)
    traverse(ast, {
      ImportDeclaration(astPath) {
        const node = astPath.node
        const source = node.source?.value
        if (typeof source !== 'string') return
        const declarationTypeOnly = node.importKind === 'type'
        for (const specifier of node.specifiers) {
          if (!t.isImportSpecifier(specifier)) continue
          if (!declarationTypeOnly && specifier.importKind !== 'type') continue
          if (!t.isIdentifier(specifier.local)) continue
          const imported = t.isIdentifier(specifier.imported) ? specifier.imported.name : specifier.imported.value
          typeImports.set(specifier.local.name, { file, source, imported, local: specifier.local.name })
        }
      },
      TSInterfaceDeclaration(astPath) {
        const node = astPath.node
        if (!node.id?.name || node.start == null || node.end == null) return
        declarations.set(node.id.name, code.slice(node.start, node.end).replace(/^export\s+/, '').trim())
      },
      TSTypeAliasDeclaration(astPath) {
        const node = astPath.node
        if (!node.id?.name || node.start == null || node.end == null) return
        declarations.set(node.id.name, code.slice(node.start, node.end).replace(/^export\s+/, '').trim())
      },
      FunctionDeclaration(astPath) {
        const node = astPath.node
        if (!node.id?.name) return
        collectParameterHints(node.id.name, node.params, code)
        if (!node.returnType?.typeAnnotation) return
        const typeNode = node.returnType.typeAnnotation
        if (typeNode.start == null || typeNode.end == null) return
        returnHints.set(node.id.name, code.slice(typeNode.start, typeNode.end).trim())
      },
      ArrowFunctionExpression(astPath) {
        collectInlineCallableHint(astPath, code)
      },
      FunctionExpression(astPath) {
        collectInlineCallableHint(astPath, code)
      },
      Class(astPath) {
        const node = astPath.node
        if (!node.id?.name) return
        if (classNoRuntimeBridgeDirective(node)) classHints.set(node.id.name, '@gea-no-runtime-bridge')
      },
      VariableDeclarator(astPath) {
        const node = astPath.node
        if (!t.isIdentifier(node.id)) return
        if (node.init && (t.isArrowFunctionExpression(node.init) || t.isFunctionExpression(node.init))) {
          collectParameterHints(node.id.name, node.init.params, code)
          const typeNode = node.init.returnType?.typeAnnotation
          if (typeNode && typeNode.start != null && typeNode.end != null) {
            returnHints.set(node.id.name, code.slice(typeNode.start, typeNode.end).trim())
          }
        }
        const jsonParseType = jsonParseCastTypeText(node, code)
        if (jsonParseType) {
          const fnName = resolveFunctionHintName(astPath)
          if (fnName) localHints.set(`${fnName}#${node.id.name}`, jsonParseType)
          return
        }
        const localTypeNode = node.id.typeAnnotation?.typeAnnotation
        if (!localTypeNode || localTypeNode.start == null || localTypeNode.end == null) return
        // Hintable arrays AND named type references (`const bytes: Uint8Array =
        // await res.arrayBuffer()`): bundling erases the annotation, the checker
        // types the local `any` → gea_cpp_value, and a native-typed initializer
        // (the fetch body's std::vector<std::uint8_t>) gets BOXED at the
        // assignment — per-byte, catastrophically. Named refs are the same hint
        // class ClassProperty already restores.
        if (!isHintableTypeNode(localTypeNode)) return
        // Module-level typed arrays (`const cacheImgs: GeaEmbeddedImage[] = []`)
        // lose their element type the same way function-local ones do, so key
        // them under a module scope rather than skipping.
        const functionName = resolveFunctionHintName(astPath) ?? MODULE_HINT_SCOPE
        localHints.set(`${functionName}#${node.id.name}`, code.slice(localTypeNode.start, localTypeNode.end).trim())
      },
      // Class method params carry the same annotations as free functions
      // (`multiply(other: Quaternion)`), but bundling strips them and the
      // checker then types `other` as `any` -> `gea_cpp_value`, losing the
      // concrete fields. Restore them as JSDoc keyed by `Class#method` so the
      // qualified name avoids collisions when two classes share a method name.
      ClassMethod(astPath) {
        const node = astPath.node
        if ((node.kind !== 'method' && node.kind !== 'constructor') || !t.isIdentifier(node.key)) return
        const classPath = astPath.findParent((p) => p.isClassDeclaration() || p.isClassExpression())
        const className = classPath?.node?.id?.name
        if (!className) return
        const key = `${className}#${node.kind === 'constructor' ? 'constructor' : node.key.name}`
        collectParameterHints(key, node.params, code)
        const typeNode = node.returnType?.typeAnnotation
        if (typeNode && typeNode.start != null && typeNode.end != null) {
          returnHints.set(key, code.slice(typeNode.start, typeNode.end).trim())
        }
      },
      // Class field annotations carry the element type of typed store arrays
      // (`hours: Forecast[]`). Bundling drops them; capture the annotation text
      // keyed by `Class#prop` so it can be re-applied as a JSDoc `@type`.
      //
      // Restricted to arrays whose element is a NAMED type reference
      // (`Forecast[]` / `Array<Forecast>`). This is exactly the typed
      // store-array reuse case the `__gea_type_<Name>` lowering targets.
      // Restoring annotations for primitive arrays (`number[]`) or scalars is
      // unnecessary here and would promote fields geatsc otherwise keeps
      // dynamic — exposing unrelated typed-storage lowering paths.
      ClassProperty(astPath) {
        const node = astPath.node
        if (node.computed || !t.isIdentifier(node.key)) return
        const typeNode = node.typeAnnotation?.typeAnnotation
        if (!typeNode || typeNode.start == null || typeNode.end == null) return
        const classPath = astPath.findParent((p) => p.isClassDeclaration() || p.isClassExpression())
        const className = classPath?.node?.id?.name
        if (!className) return
        // Named-element arrays (`Forecast[]`) and bare named-type fields (a host
        // handle like `drone: GeaEmbeddedImage`) keep their static type.
        // Primitive arrays (`number[]`) are restored too: the element-write
        // lowering gap that once forced them dynamic (`q[i] = q[i+1]` swaps
        // miscompiling on a typed field) is fixed — geatsc now lowers typed
        // field element writes through `gea::runtime::array::vector_set`
        // (pinned by compiler-js-callback-param-typing.test.ts's sibling
        // class-field cases). Without the restoration, a `fetchQueue:
        // number[]` store field stays `std::vector<gea_cpp_value>` and every
        // queue operation boxes.
        //
        // `@gea-no-runtime-bridge` marks hand-written native adapter classes.
        // Their annotated scalar fields are part of the typed ABI boundary
        // (`__nativeHandle: number`, `canvasWidth: () => number`, etc.), so
        // restore every explicit field annotation for those classes only.
        const restoreAllAnnotatedFields = classNoRuntimeBridgeDirective(classPath.node)
        if (
          !restoreAllAnnotatedFields &&
          !isNamedInterfaceArrayTypeNode(typeNode) &&
          !isNamedTypeReferenceNode(typeNode) &&
          !isPrimitiveArrayTypeNode(typeNode)
        ) return
        propertyHints.set(`${className}#${node.key.name}`, code.slice(typeNode.start, typeNode.end).trim())
      },
      // A `class X extends ReactiveComponent<T>` (or `Component<T>`) gives
      // `this.el` the element type `T | null` (Component<RootElement> declares
      // `el: RootElement | null`). The lean ReactiveComponent transform strips
      // the base and its inherited `el`; the framework re-injects a plain `el`
      // field (vendor/gea transform.ts), but bundling leaves it UNTYPED so geatsc
      // boxes it to `gea_cpp_value` — and a typed canvas element must stay native
      // (it maps to `gea::embedded::ui::NodeHandle`, so `getContext('2d')` and the
      // mount's root binding flow with zero boxing). `el` has no source field to
      // annotate, so recover `T` from the heritage generic and key the hint under
      // `Class#el`; the ClassProperty restorer then stamps `@type {T | null}` onto
      // the injected field. Mirrors a non-stripped `Component<T>` whose `el`
      // already lowers to a NodeHandle via nativeTypes.
      Class(astPath) {
        const node = astPath.node
        if (!t.isIdentifier(node.id) || !t.isIdentifier(node.superClass)) return
        if (node.superClass.name !== 'ReactiveComponent' && node.superClass.name !== 'Component') return
        const typeArgs = node.superTypeParameters ?? node.superTypeArguments
        const typeArg = typeArgs?.params?.[0]
        if (!typeArg || typeArg.start == null || typeArg.end == null) return
        propertyHints.set(`${node.id.name}#el`, `${code.slice(typeArg.start, typeArg.end).trim()} | null`)
      },
    })
  }

  const neededTypeNames = new Set()
  for (const returnType of returnHints.values()) {
    for (const name of declarations.keys()) {
      if (typeTextReferencesName(returnType, name)) neededTypeNames.add(name)
    }
  }
  for (const params of paramHints.values()) {
    for (const param of params) {
      for (const name of declarations.keys()) {
        if (typeTextReferencesName(param.type, name)) neededTypeNames.add(name)
      }
    }
  }
  for (const propertyType of propertyHints.values()) {
    for (const name of declarations.keys()) {
      if (typeTextReferencesName(propertyType, name)) neededTypeNames.add(name)
    }
  }
  for (const localType of localHints.values()) {
    for (const name of declarations.keys()) {
      if (typeTextReferencesName(localType, name)) neededTypeNames.add(name)
    }
  }
  // Transitively pull in type names referenced *inside* an already-needed
  // declaration. `type ControlButton = { id: ControlButtonId }` is named only
  // by a `@returns {ControlButton[]}`; without following the reference to
  // `ControlButtonId`, that nested alias is dropped from the sidecar and the
  // checker types `id` as `any` -> gea_cpp_value (losing static lowering and,
  // for sky-hop, exposing a heap-corruption crash on the dynamic value's
  // destructor). Fixpoint over declaration bodies so the whole referenced
  // closure (nested records, `A & B` intersections, string-literal unions) is
  // preserved. Self-references terminate because the set only grows and is
  // bounded by `declarations`.
  let addedTransitive = true
  while (addedTransitive) {
    addedTransitive = false
    for (const needed of [...neededTypeNames]) {
      const declText = declarations.get(needed)
      if (!declText) continue
      for (const name of declarations.keys()) {
        if (neededTypeNames.has(name)) continue
        if (typeTextReferencesName(declText, name)) {
          neededTypeNames.add(name)
          addedTransitive = true
        }
      }
    }
  }
  if (
    returnHints.size === 0 &&
    paramHints.size === 0 &&
    classHints.size === 0 &&
    propertyHints.size === 0 &&
    localHints.size === 0 &&
    inlineCallableHints.size === 0
  ) {
    return { imports: [], declarations: [], returnHints, paramHints, classHints, propertyHints, localHints, inlineCallableHints }
  }
  const declarationTexts = [...neededTypeNames].map((name) => declarations.get(name)).filter(Boolean)
  // Names emitted as LOCAL declarations in the sidecar. A name re-emitted as
  // BOTH a local declaration and a re-imported type collides (two competing
  // symbols for the same name). Subtract them: a locally-declared interface
  // (`interface CityState {…}`) trivially matches `/\bCityState\b/` against its
  // OWN declaration text, so without this guard `typeTextReferencesName` would
  // wrongly re-emit `import type { CityState }` next to the local declaration.
  const locallyDeclaredNames = new Set(
    [...neededTypeNames].filter((name) => declarations.get(name))
  )
  // A hint can reference an EXTERNAL type directly (not via a local declaration)
  // — e.g. `const cacheImgs: GeaEmbeddedImage[] = []` where `GeaEmbeddedImage` is
  // a type-only import from 'gea-embedded'. Re-import any such type so the
  // restored `@type {GeaEmbeddedImage[]}` resolves in the sidecar.
  const hintTypeTexts = [
    ...returnHints.values(),
    ...[...paramHints.values()].flatMap((params) => params.map((param) => param.type)),
    ...propertyHints.values(),
    ...localHints.values(),
    ...[...inlineCallableHints.values()].flatMap((entries) =>
      entries.flatMap((entry) => [...entry.params.map((param) => param.type), ...(entry.returnType ? [entry.returnType] : [])])
    ),
  ]
  const referencedImports = [...typeImports].flatMap(([local, info]) =>
    !locallyDeclaredNames.has(local) &&
    (declarationTexts.some((declText) => typeTextReferencesName(declText, local)) ||
      hintTypeTexts.some((hintText) => typeTextReferencesName(hintText, local)))
      ? [info]
      : []
  )
  return {
    imports: renderTypeImports(referencedImports),
    declarations: declarationTexts,
    returnHints,
    paramHints,
    classHints,
    propertyHints,
    localHints,
    inlineCallableHints,
  }
}

function writeAppleNativeViteConfig({ configPath, baseConfigPath, appDir, moduleGraphOutDir }) {
  const applePackageRoot = resolveApplePackageRoot(appDir)
  const appleRuntimeSubpath = path.join(applePackageRoot, 'runtime/$1.js')
  const applePackagePath = path.join(applePackageRoot, 'dist/index.js')
  const moduleGraphPluginPath = path.join(coreRoot, 'scripts/gea-vite-module-graph-plugin.mjs')
  const { threeSrcDir, threeUtilsModule, threeWebGLAnimationModule, threeWebXRManagerModule, troikaThreeTextModule } = resolveAppleNativeAliasModules(appDir)
  const q = (value) => JSON.stringify(value)
  fs.writeFileSync(
    configPath,
    `import baseConfig from ${q(baseConfigPath)}
import { geaAppleNativeModuleAliases, geaModuleGraphPlugins } from ${q(moduleGraphPluginPath)}

const appleAliases = [
  { find: /^@geajs\\/apple\\/(.+)$/, replacement: ${q(appleRuntimeSubpath)} },
  { find: /^@geastack\\/apple\\/(.+)$/, replacement: ${q(appleRuntimeSubpath)} },
  { find: '@geajs/apple', replacement: ${q(applePackagePath)} },
  { find: '@geastack/apple', replacement: ${q(applePackagePath)} },
]
const geaModuleGraph = geaModuleGraphPlugins({ outDir: ${q(moduleGraphOutDir || '')}, entryReachableOnly: true })
const geaAppleNativeAliases = geaAppleNativeModuleAliases({
  threeSrcDir: ${q(threeSrcDir)},
  threeUtilsModule: ${q(threeUtilsModule)},
  threeWebGLAnimationModule: ${q(threeWebGLAnimationModule)},
  threeWebXRManagerModule: ${q(threeWebXRManagerModule)},
  troikaThreeTextModule: ${q(troikaThreeTextModule)},
})

function normalizeAlias(alias) {
  if (!alias) return []
  if (Array.isArray(alias)) return alias
  return Object.entries(alias).map(([find, replacement]) => ({ find, replacement }))
}

export default async function geaAppleNativeViteConfig(env) {
  const resolved = typeof baseConfig === 'function' ? await baseConfig(env) : baseConfig
  const baseResolve = resolved.resolve ?? {}
  return {
    ...resolved,
    resolve: {
      ...baseResolve,
      alias: [...geaAppleNativeAliases, ...appleAliases, ...normalizeAlias(baseResolve.alias)],
    },
    plugins: [...(resolved.plugins ?? []), ...geaModuleGraph],
  }
}
`
  )
}

// Frameworks that only exist on the *other* Apple platform. The fixture ships
// every framework (UIKit + AppKit), but UIKit and AppKit declare members with the
// same names (font, textColor, hidden, addSubview, ...). When a native member is
// accessed on a dynamic gea_cpp_value receiver the emitter can't tell which class
// it is, so it picks the first registered binding — UIKit's `::UILabel`/`::UIFont`
// etc., which don't exist in a macOS (AppKit) build and fail to compile.
const applePlatformOffFrameworks = {
  macos: new Set(['UIKit']),
  ios: new Set(['AppKit']),
}

async function writeAppleNativeMetadata(outDir, appDir, platform = '') {
  const applePackage = path.join(resolveApplePackageRoot(appDir), 'dist/index.js')
  if (!fs.existsSync(applePackage)) {
    fail(`missing @geastack/apple build output: ${applePackage}. Run the Apple bindings build first.`)
  }
  const { appleSdkFixture, generateAppleBridgeMetadata } = await import(pathToFileURL(applePackage).href)
  const offPlatform = applePlatformOffFrameworks[platform] ?? new Set()
  // Keep the off-platform class SHELLS — other frameworks extend them (e.g.
  // MKMapView/MTKView extend UIKit.UIView) so the wrapper structs must still be
  // declared — but strip their members, constructors, free functions and
  // constants. Shared member names then register a binding only for the
  // on-platform class, so native access on a dynamic receiver resolves to the
  // platform's class instead of leaking an off-platform ObjC type.
  const fixture =
    offPlatform.size > 0
      ? {
          ...appleSdkFixture,
          frameworks: appleSdkFixture.frameworks.map((framework) =>
            offPlatform.has(framework.name)
              ? {
                  ...framework,
                  classes: (framework.classes ?? []).map((cls) => ({ ...cls, constructors: [], methods: [], properties: [] })),
                  functions: [],
                  constants: [],
                }
              : framework
          ),
        }
      : appleSdkFixture
  const metadataPath = path.join(outDir, 'gea-apple-metadata.json')
  fs.writeFileSync(metadataPath, `${JSON.stringify(generateAppleBridgeMetadata(fixture), null, 2)}\n`)
  return metadataPath
}

function collectMemberPath(node) {
  if (t.isIdentifier(node)) return [node.name]
  if (!t.isMemberExpression(node) || node.computed) return null
  const objectPath = collectMemberPath(node.object)
  if (!objectPath || !t.isIdentifier(node.property)) return null
  objectPath.push(node.property.name)
  return objectPath
}

function evalStaticLiteral(node) {
  if (t.isBooleanLiteral(node) || t.isNumericLiteral(node) || t.isStringLiteral(node)) return node.value
  if (t.isNullLiteral(node)) return null
  if (t.isUnaryExpression(node, { operator: '-' })) {
    const value = evalStaticLiteral(node.argument)
    if (typeof value === 'number') return -value
  }
  if (t.isUnaryExpression(node, { operator: '+' })) {
    const value = evalStaticLiteral(node.argument)
    if (typeof value === 'number') return value
  }
  return undefined
}

function evalStaticObject(node) {
  if (!t.isObjectExpression(node)) return null
  const result = {}
  for (const prop of node.properties) {
    if (!t.isObjectProperty(prop) || prop.computed) return null
    const key = t.isIdentifier(prop.key) ? prop.key.name : t.isStringLiteral(prop.key) ? prop.key.value : null
    if (!key) return null
    const value = evalStaticLiteral(prop.value)
    if (value === undefined) return null
    result[key] = value
  }
  return result
}

function collectDisplayConfig(appDir, entry) {
  const pending = [path.resolve(appDir, entry)]
  const processed = new Set()
  const displayConfig = {}

  while (pending.length > 0) {
    const srcId = pending.shift()
    if (!srcId || processed.has(srcId) || !fs.existsSync(srcId)) continue
    processed.add(srcId)

    // Only parse JS/TS modules. The import walk follows every local import,
    // including asset imports (e.g. troika font `import url from './x.woff'`);
    // feeding a binary asset to the TS parser blows up on its first non-ASCII
    // byte (a .woff's `wOFF` header → "Unexpected character" at column 4).
    // Assets carry no Display config, so skip them.
    if (!/\.(?:mjs|cjs|jsx?|tsx?)$/i.test(srcId)) continue

    const code = fs.readFileSync(srcId, 'utf8')
    const ast = parseTsx(code)
    const displayVars = new Set()

    traverse(ast, {
      ImportDeclaration(astPath) {
        const source = astPath.node.source.value
        if (source.includes('@geastack/core')) {
          for (const spec of astPath.node.specifiers) {
            if (t.isImportSpecifier(spec) && t.isIdentifier(spec.imported, { name: 'Display' }) && t.isIdentifier(spec.local)) {
              displayVars.add(spec.local.name)
            }
          }
        }
        discoverLocalImport(source, srcId, pending)
      },
    })

    traverse(ast, {
      ExpressionStatement(astPath) {
        if (astPath.parent.type !== 'Program') return
        const expr = astPath.node.expression
        if (!t.isCallExpression(expr)) return
        const memberPath = collectMemberPath(expr.callee)
        if (!memberPath || memberPath.length !== 2 || !displayVars.has(memberPath[0])) return
        const config = evalStaticObject(expr.arguments[0])
        if (!config) return
        if (memberPath[1] === 'setFlushConfig') {
          const chunkRows = positiveInteger(config.rows)
          const queueDepth = positiveInteger(config.depth)
          if (chunkRows > 0) displayConfig.flushChunkRows = chunkRows
          if (queueDepth > 0) displayConfig.flushQueueDepth = queueDepth
        } else if (memberPath[1] === 'setMemoryConfig') {
          const commandBufferCommands = positiveInteger(config.commandBufferCommands)
          if (commandBufferCommands > 0) displayConfig.commandBufferCommands = commandBufferCommands
          if (typeof config.backgroundCache === 'boolean') displayConfig.backgroundCache = config.backgroundCache
        }
      },
    })
  }

  return displayConfig
}

function positiveInteger(value) {
  if (typeof value !== 'number') return 0
  const rounded = Math.floor(value)
  return rounded > 0 ? rounded : 0
}

function writeDisplayConfigHeader(outDir, displayDefaults) {
  const lines = ['#pragma once']
  const chunkRows = positiveInteger(displayDefaults.flushChunkRows)
  const queueDepth = positiveInteger(displayDefaults.flushQueueDepth)
  const commandBufferCommands = positiveInteger(displayDefaults.commandBufferCommands)
  if (chunkRows > 0) lines.push(`#define GEA_EMBEDDED_DISPLAY_FLUSH_CHUNK_MAX ${chunkRows}`)
  if (queueDepth > 0) lines.push(`#define GEA_EMBEDDED_DISPLAY_FLUSH_QUEUE_DEPTH ${queueDepth}`)
  if (commandBufferCommands > 0) lines.push(`#define GEA_EMBEDDED_DISPLAY_COMMAND_BUFFER_COMMANDS ${commandBufferCommands}`)
  if (typeof displayDefaults.backgroundCache === 'boolean') {
    lines.push(`#define GEA_EMBEDDED_DISPLAY_BACKGROUND_CACHE_IN_SRAM ${displayDefaults.backgroundCache ? 1 : 0}`)
  }
  writeFileIfChanged(path.join(outDir, 'gea_embedded_app_config.h'), `${lines.join('\n')}\n`)
}

// Parse plain CSS rules (already free of @media) into StyleSheet registration
// calls. When `mediaCondition` is non-empty, every emitted rule is gated on it
// and re-evaluated against the viewport at runtime.
function cssRegisterCall(kind) {
  if (useStaticCssRules) {
    if (kind === 'class') return 'registerStaticRule'
    if (kind === 'element') return 'registerStaticElementRule'
    if (kind === 'selector') return 'registerStaticSelectorRule'
    if (kind === 'keyframe') return 'registerStaticKeyframeRule'
  }
  if (kind === 'class') return 'registerRule'
  if (kind === 'element') return 'registerElementRule'
  if (kind === 'selector') return 'registerSelectorRule'
  return 'registerKeyframeRule'
}

function staticSelectorKind(kind) {
  if (kind === 'class') return 'gea::embedded::ui::StaticStyleSelectorKind::Class'
  if (kind === 'element') return 'gea::embedded::ui::StaticStyleSelectorKind::Element'
  return 'gea::embedded::ui::StaticStyleSelectorKind::Selector'
}

let staticCssSelectorPlanCache = new Set()
let staticCssMediaConditionPlanCache = new Set()

function stripStaticSelectorPseudoElement(selector) {
  return selector.replace(/::(?:before|after)\s*$/i, '').trim()
}

function parseStaticSimpleSelectorSpec(rawSimple) {
  const simple = rawSimple.trim()
  if (!simple || simple.includes('::')) return null
  const spec = {
    tag: null,
    id: null,
    classes: [],
    wantsRoot: false,
    wantsFirstChild: false,
    wantsLastChild: false,
    wantsHover: false,
  }
  let i = 0
  if (simple[i] !== '.' && simple[i] !== '#' && simple[i] !== ':') {
    const start = i
    while (i < simple.length && simple[i] !== '.' && simple[i] !== '#' && simple[i] !== ':') i++
    const tag = simple.slice(start, i).trim().toLowerCase()
    if (!tag) return null
    spec.tag = tag
  }
  while (i < simple.length) {
    const marker = simple[i++]
    const start = i
    while (i < simple.length && simple[i] !== '.' && simple[i] !== '#' && simple[i] !== ':') i++
    const token = simple.slice(start, i).trim()
    if (!token) return null
    if (marker === '.') {
      spec.classes.push(token)
    } else if (marker === '#') {
      spec.id = token
    } else if (marker === ':') {
      if (token === 'root') spec.wantsRoot = true
      else if (token === 'first-child') spec.wantsFirstChild = true
      else if (token === 'last-child') spec.wantsLastChild = true
      else if (token === 'hover') spec.wantsHover = true
      else return null
    } else {
      return null
    }
  }
  if (!spec.tag && !spec.id && spec.classes.length === 0 &&
      !spec.wantsRoot && !spec.wantsFirstChild && !spec.wantsLastChild && !spec.wantsHover) {
    return null
  }
  return spec
}

function parseStaticSelectorPlanSpec(selector) {
  const text = stripStaticSelectorPseudoElement(selector)
  if (!text) return null
  const parts = []
  let nextDirect = false
  let i = 0
  while (i < text.length) {
    while (i < text.length && text.charCodeAt(i) <= 32) i++
    if (i >= text.length) break
    if (text[i] === '>') {
      nextDirect = true
      i++
      continue
    }
    const start = i
    while (i < text.length && text.charCodeAt(i) > 32 && text[i] !== '>') i++
    if (i <= start) continue
    const simple = parseStaticSimpleSelectorSpec(text.slice(start, i))
    if (!simple) return null
    parts.push({ simple, directParent: nextDirect })
    nextDirect = false
  }
  return parts.length > 0 ? { text, parts } : null
}

function staticSimpleSelectorSpecCode(simple) {
  const tag = simple.tag ? JSON.stringify(simple.tag) : 'nullptr'
  const id = simple.id ? JSON.stringify(simple.id) : 'nullptr'
  const classes = simple.classes.map((className) => JSON.stringify(className)).join(', ')
  return `{${tag}, ${id}, {${classes}}, ${simple.wantsRoot ? 'true' : 'false'}, ${simple.wantsFirstChild ? 'true' : 'false'}, ${simple.wantsLastChild ? 'true' : 'false'}, ${simple.wantsHover ? 'true' : 'false'}}`
}

function staticSelectorPartSpecCode(part) {
  return `{${staticSimpleSelectorSpecCode(part.simple)}, ${part.directParent ? 'true' : 'false'}}`
}

function staticSelectorPlanRegistration(selector) {
  if (!useStaticCssRules) return []
  const plan = parseStaticSelectorPlanSpec(selector)
  if (!plan) return []
  if (staticCssSelectorPlanCache.has(plan.text)) return []
  staticCssSelectorPlanCache.add(plan.text)
  const parts = plan.parts.map(staticSelectorPartSpecCode).join(', ')
  return [
    `__gea_stylesheet.registerStaticSelectorPlan(${JSON.stringify(selector)}, {${parts}});`,
  ]
}

function parseStaticMediaNumber(raw) {
  const value = raw.trim()
  if (!value) return null
  const slash = value.indexOf('/')
  if (slash >= 0) {
    const width = Number.parseFloat(value.slice(0, slash))
    const height = Number.parseFloat(value.slice(slash + 1))
    if (!Number.isFinite(width) || !Number.isFinite(height) || height === 0) return null
    return width / height
  }
  const match = value.match(/^([+-]?(?:\d+(?:\.\d*)?|\.\d+))(.*)$/)
  if (!match) return null
  const number = Number(match[1])
  if (!Number.isFinite(number)) return null
  const unit = match[2].trim().toLowerCase()
  if (unit === 'dpi') return number / 96
  return number
}

function staticMediaFeatureForName(name) {
  const feature = name.trim().toLowerCase()
  const exact = (kind) => ({ kind, compare: 'Equal' })
  const min = (kind) => ({ kind, compare: 'Min' })
  const max = (kind) => ({ kind, compare: 'Max' })
  switch (feature) {
    case 'orientation': return exact('Orientation')
    case 'monochrome': return exact('Monochrome')
    case 'min-monochrome': return min('Monochrome')
    case 'max-monochrome': return max('Monochrome')
    case 'width': return exact('Width')
    case 'min-width': return min('Width')
    case 'max-width': return max('Width')
    case 'height': return exact('Height')
    case 'min-height': return min('Height')
    case 'max-height': return max('Height')
    case 'aspect-ratio': return exact('AspectRatio')
    case 'min-aspect-ratio': return min('AspectRatio')
    case 'max-aspect-ratio': return max('AspectRatio')
    case 'resolution':
    case 'device-pixel-ratio':
      return exact('Resolution')
    case 'min-resolution':
    case 'min-device-pixel-ratio':
      return min('Resolution')
    case 'max-resolution':
    case 'max-device-pixel-ratio':
      return max('Resolution')
    default:
      return null
  }
}

function alwaysFalseStaticMediaTerm() {
  return { kind: 'AlwaysFalse', compare: 'Equal', value: 0, orientation: 0 }
}

function compileStaticMediaFeatureTerm(name, rawValue) {
  const feature = staticMediaFeatureForName(name)
  if (!feature) return alwaysFalseStaticMediaTerm()
  if (feature.kind === 'Orientation') {
    const orientation = rawValue.trim().toLowerCase()
    if (orientation === 'portrait') return { ...feature, value: 0, orientation: 1 }
    if (orientation === 'landscape') return { ...feature, value: 0, orientation: 2 }
    return alwaysFalseStaticMediaTerm()
  }
  const value = parseStaticMediaNumber(rawValue)
  if (value === null) return alwaysFalseStaticMediaTerm()
  return { ...feature, value, orientation: 0 }
}

function parseStaticMediaQuerySpec(rawQuery) {
  const query = rawQuery.trim()
  if (!query) return { valid: false, terms: [] }
  const terms = []
  let i = 0
  while (true) {
    const open = query.indexOf('(', i)
    if (open < 0) break
    const close = query.indexOf(')', open)
    if (close < 0) return { valid: false, terms: [] }
    const feature = query.slice(open + 1, close)
    const colon = feature.indexOf(':')
    if (colon >= 0) {
      terms.push(compileStaticMediaFeatureTerm(feature.slice(0, colon), feature.slice(colon + 1)))
    } else if (feature.trim().toLowerCase() === 'monochrome') {
      terms.push({ kind: 'Monochrome', compare: 'Boolean', value: 0, orientation: 0 })
    }
    i = close + 1
  }
  return { valid: true, terms }
}

function staticMediaTermSpecCode(term) {
  return `{gea::embedded::ui::StaticStyleMediaFeatureKind::${term.kind}, gea::embedded::ui::StaticStyleMediaCompare::${term.compare}, ${term.value}, ${term.orientation}}`
}

function staticMediaQuerySpecCode(query) {
  return `{{${query.terms.map(staticMediaTermSpecCode).join(', ')}}, ${query.valid ? 'true' : 'false'}}`
}

function staticMediaConditionPlanRegistration(condition) {
  if (!useStaticCssRules || !condition) return []
  if (staticCssMediaConditionPlanCache.has(condition)) return []
  const queries = splitTopLevelCss(condition, ',').map(parseStaticMediaQuerySpec)
  if (queries.length === 0) return []
  staticCssMediaConditionPlanCache.add(condition)
  return [
    `__gea_stylesheet.registerStaticMediaConditionPlan(${JSON.stringify(condition)}, {${queries.map(staticMediaQuerySpecCode).join(', ')}});`,
  ]
}

function roundCssNumber(value) {
  return value < 0 ? Math.ceil(value - 0.5) : Math.floor(value + 0.5)
}

function clampInt16(value) {
  if (value < -32768) return -32768
  if (value > 32767) return 32767
  return value
}

function simpleStaticLength(value) {
  const match = value.match(/^([+-]?(?:\d+(?:\.\d*)?|\.\d+))(px|%)?$/i)
  if (!match) return null
  const number = Number(match[1])
  if (!Number.isFinite(number)) return null
  const unit = (match[2] || '').toLowerCase()
  return { number, unit }
}

function parseStaticCssLength(rawValue) {
  const value = rawValue.trim()
  const match = value.match(/^([+-]?(?:\d+(?:\.\d*)?|\.\d+))(px|%|vw|vh|vmin|vmax|dvw|dvh)?$/i)
  if (!match) return null
  let number = Number(match[1])
  if (!Number.isFinite(number)) return null
  if (Object.is(number, -0)) number = 0
  const unit = (match[2] || '').toLowerCase()
  const units = new Map([
    ['', 'Raw'],
    ['px', 'Px'],
    ['%', 'Percent'],
    ['vw', 'Vw'],
    ['vh', 'Vh'],
    ['vmin', 'Vmin'],
    ['vmax', 'Vmax'],
    ['dvw', 'Dvw'],
    ['dvh', 'Dvh'],
  ])
  return units.has(unit) ? { number, unit: units.get(unit) } : null
}

let staticCssLengthExpressionCounter = 0
let staticCssLengthExpressionCache = new Map()

function cssNumberCode(value) {
  if (!Number.isFinite(value)) return '0'
  return String(Object.is(value, -0) ? 0 : value)
}

function staticLengthSpecCode(spec) {
  return `{gea::embedded::ui::StaticStyleLengthUnit::${spec.unit}, ${spec.value}}`
}

function zeroStaticLengthSpec() {
  return { unit: 'Raw', value: '0' }
}

function staticCssFunctionInner(rawValue, name) {
  const value = rawValue.trim()
  const lower = value.toLowerCase()
  const prefix = `${name}(`
  if (!lower.startsWith(prefix) || !value.endsWith(')')) return null
  return value.slice(prefix.length, -1)
}

function findTopLevelCssOperator(value, operators) {
  let depth = 0
  for (let i = 0; i < value.length; i++) {
    const char = value[i]
    if (char === '(') {
      depth++
      continue
    }
    if (char === ')') {
      if (depth > 0) depth--
      continue
    }
    if (depth !== 0 || !operators.includes(char) || i === 0) continue
    if ((char === '+' || char === '-') && /[eE]/.test(value[i - 1] ?? '')) continue
    return i
  }
  return -1
}

function emitStaticLengthExpression(kind, specs, options = {}) {
  const key = options.key ?? `${kind}:${specs.map((spec) => `${spec.unit}:${spec.value}`).join('|')}:${options.scalar ?? 0}:${options.name ?? ''}:${options.hasFallback ? 1 : 0}`
  const cached = staticCssLengthExpressionCache.get(key)
  if (cached) return { setup: [], spec: { unit: 'Expression', value: `static_cast<float>(${cached})` } }
  const name = `gea_css_len_expr_${staticCssLengthExpressionCounter++}`
  staticCssLengthExpressionCache.set(key, name)
  const empty = zeroStaticLengthSpec()
  const a = specs[0] ?? empty
  const b = specs[1] ?? empty
  const c = specs[2] ?? empty
  const expressionName = options.name ? JSON.stringify(options.name) : 'nullptr'
  const scalar = cssNumberCode(options.scalar ?? 0)
  const hasFallback = options.hasFallback ? 'true' : 'false'
  const call = `const std::uint16_t ${name} = __gea_stylesheet.registerStaticLengthExpression(gea::embedded::ui::StaticStyleLengthExpressionKind::${kind}, ${staticLengthSpecCode(a)}, ${staticLengthSpecCode(b)}, ${staticLengthSpecCode(c)}, ${scalar}, ${expressionName}, ${hasFallback});`
  return { setup: [call], spec: { unit: 'Expression', value: `static_cast<float>(${name})` } }
}

function parseStaticCssLengthSpec(rawValue) {
  const value = rawValue.trim()
  const simple = parseStaticCssLength(value)
  if (simple) return { setup: [], spec: { unit: simple.unit, value: cssNumberCode(simple.number) } }

  const varInner = staticCssFunctionInner(value, 'var')
  if (varInner !== null) {
    const parts = splitTopLevelCss(varInner, ',')
    if (parts.length < 1 || parts.length > 2) return null
    const name = parts[0].trim()
    if (!/^--[A-Za-z0-9_-]+$/.test(name)) return null
    const fallback = parts.length === 2 ? parseStaticCssLengthSpec(parts[1]) : null
    if (parts.length === 2 && !fallback) return null
    const emitted = emitStaticLengthExpression('Var',
      [fallback?.spec ?? zeroStaticLengthSpec()],
      { name, hasFallback: Boolean(fallback), key: `var:${name}:${fallback ? `${fallback.spec.unit}:${fallback.spec.value}` : ''}` })
    return { setup: [...(fallback?.setup ?? []), ...emitted.setup], spec: emitted.spec }
  }

  const clampInner = staticCssFunctionInner(value, 'clamp')
  if (clampInner !== null) {
    const parts = splitTopLevelCss(clampInner, ',')
    if (parts.length !== 3) return null
    const parsed = parts.map(parseStaticCssLengthSpec)
    if (parsed.some((entry) => !entry)) return null
    const emitted = emitStaticLengthExpression('Clamp',
      parsed.map((entry) => entry.spec),
      { key: `clamp:${parts.map((part) => part.trim()).join('|')}` })
    return { setup: [...parsed.flatMap((entry) => entry.setup), ...emitted.setup], spec: emitted.spec }
  }

  for (const [functionName, kind] of [['min', 'Min'], ['max', 'Max']]) {
    const inner = staticCssFunctionInner(value, functionName)
    if (inner === null) continue
    const parts = splitTopLevelCss(inner, ',')
    if (parts.length < 1) return null
    const parsed = parts.map(parseStaticCssLengthSpec)
    if (parsed.some((entry) => !entry)) return null
    let setup = parsed[0].setup.slice()
    let current = parsed[0].spec
    for (let i = 1; i < parsed.length; i++) {
      setup.push(...parsed[i].setup)
      const emitted = emitStaticLengthExpression(kind,
        [current, parsed[i].spec],
        { key: `${functionName}:${parts.slice(0, i + 1).map((part) => part.trim()).join('|')}` })
      setup.push(...emitted.setup)
      current = emitted.spec
    }
    return { setup, spec: current }
  }

  const calcInner = staticCssFunctionInner(value, 'calc')
  if (calcInner !== null) {
    const text = calcInner.trim()
    for (const [operators, kinds] of [
      ['/*', { '/': 'Divide', '*': 'Multiply' }],
      ['+-', { '+': 'Add', '-': 'Subtract' }],
    ]) {
      const index = findTopLevelCssOperator(text, operators)
      if (index <= 0) continue
      const op = text[index]
      const left = parseStaticCssLengthSpec(text.slice(0, index))
      if (!left) return null
      if (op === '/' || op === '*') {
        const scalar = Number(text.slice(index + 1).trim())
        if (!Number.isFinite(scalar)) return null
        const emitted = emitStaticLengthExpression(kinds[op],
          [left.spec],
          { scalar, key: `calc:${text}` })
        return { setup: [...left.setup, ...emitted.setup], spec: emitted.spec }
      }
      const right = parseStaticCssLengthSpec(text.slice(index + 1))
      if (!right) return null
      const emitted = emitStaticLengthExpression(kinds[op],
        [left.spec, right.spec],
        { key: `calc:${text}` })
      return { setup: [...left.setup, ...right.setup, ...emitted.setup], spec: emitted.spec }
    }
    return parseStaticCssLengthSpec(text)
  }

  return null
}

function cssAlignKeywordValue(value) {
  const align = new Map([
    ['flex-start', 6],
    ['start', 6],
    ['stretch', 0],
    ['normal', 0],
    ['center', 1],
    ['flex-end', 2],
    ['end', 2],
    ['space-between', 3],
    ['space-around', 4],
    ['baseline', 5],
  ])
  return align.has(value) ? align.get(value) : null
}

function clampCssByte(value) {
  if (value < 0) return 0
  if (value > 255) return 255
  return value
}

function parseCssAlpha(value) {
  let alpha = Number(value)
  if (!Number.isFinite(alpha)) return null
  if (alpha <= 1) alpha *= 255
  return clampCssByte(Math.floor(alpha + 0.5))
}

function parseStaticCssColor(rawValue) {
  const value = rawValue.trim()
  if (value.toLowerCase() === 'transparent') return { r: 0, g: 0, b: 0, a: 0 }
  const hex = value.match(/^#([0-9a-fA-F]{3}|[0-9a-fA-F]{4}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/)
  if (hex) {
    const text = hex[1]
    if (text.length === 3 || text.length === 4) {
      const r = parseInt(text[0] + text[0], 16)
      const g = parseInt(text[1] + text[1], 16)
      const b = parseInt(text[2] + text[2], 16)
      const a = text.length === 4 ? parseInt(text[3] + text[3], 16) : 255
      return { r, g, b, a }
    }
    const r = parseInt(text.slice(0, 2), 16)
    const g = parseInt(text.slice(2, 4), 16)
    const b = parseInt(text.slice(4, 6), 16)
    const a = text.length === 8 ? parseInt(text.slice(6, 8), 16) : 255
    return { r, g, b, a }
  }
  const rgb = value.match(/^rgba?\((.*)\)$/i)
  if (!rgb) return null
  const parts = splitTopLevelCss(rgb[1], ',')
  if (parts.length < 3 || parts.length > 4) return null
  const channels = parts.slice(0, 3).map((part) => Number(part.trim()))
  if (channels.some((channel) => !Number.isFinite(channel))) return null
  const alpha = parts.length === 4 ? parseCssAlpha(parts[3].trim()) : 255
  if (alpha === null) return null
  return {
    r: clampCssByte(Math.trunc(channels[0])),
    g: clampCssByte(Math.trunc(channels[1])),
    b: clampCssByte(Math.trunc(channels[2])),
    a: alpha,
  }
}

function cssStaticColorTarget(property) {
  switch (property) {
    case 'color':
      return 'Color'
    case 'background':
      return 'Background'
    case 'background-color':
      return 'BackgroundColor'
    case 'active-background':
    case 'active-background-color':
      return 'ActiveBackground'
    case 'border-color':
      return 'Border'
    case 'border-top-color':
      return 'BorderTop'
    case 'border-right-color':
      return 'BorderRight'
    case 'border-bottom-color':
      return 'BorderBottom'
    case 'border-left-color':
      return 'BorderLeft'
    default:
      return null
  }
}

function cssStaticColorValue(property, rawValue) {
  const target = cssStaticColorTarget(property)
  if (!target) return null
  const color = parseStaticCssColor(rawValue)
  return color ? { target, color } : null
}

function cssStaticColorVarValue(property, rawValue) {
  const target = cssStaticColorTarget(property)
  if (!target) return null
  const inner = staticCssFunctionInner(rawValue.trim(), 'var')
  if (inner === null) return null
  const parts = splitTopLevelCss(inner, ',')
  if (parts.length < 1 || parts.length > 2) return null
  const name = parts[0].trim()
  if (!/^--[A-Za-z0-9_-]+$/.test(name)) return null
  const fallback = parts.length === 2 ? parseStaticCssColor(parts[1]) : null
  if (parts.length === 2 && !fallback) return null
  return { target, name, hasFallback: Boolean(fallback), color: fallback ?? { r: 0, g: 0, b: 0, a: 255 } }
}

function parseStaticCssColorRef(rawValue) {
  const color = parseStaticCssColor(rawValue)
  if (color) return { name: null, hasFallback: true, color }
  const inner = staticCssFunctionInner(rawValue.trim(), 'var')
  if (inner === null) return null
  const parts = splitTopLevelCss(inner, ',')
  if (parts.length < 1 || parts.length > 2) return null
  const name = parts[0].trim()
  if (!/^--[A-Za-z0-9_-]+$/.test(name)) return null
  const fallback = parts.length === 2 ? parseStaticCssColor(parts[1]) : null
  if (parts.length === 2 && !fallback) return null
  return { name, hasFallback: Boolean(fallback), color: fallback ?? { r: 0, g: 0, b: 0, a: 255 } }
}

function cssIgnoredProperty(property) {
  return new Set([
    'color-scheme',
    'grid-column',
    'isolation',
    'letter-spacing',
    'outline',
    'scroll-snap-align',
    'scroll-snap-type',
    'scrollbar-width',
    'text-shadow',
    'transition',
    'cursor',
    '-webkit-tap-highlight-color',
  ]).has(property)
}

function cssStaticFontWeightValue(rawValue) {
  const value = rawValue.trim().toLowerCase()
  if (!value || value === 'normal') return 400
  if (value === 'bold' || value === 'bolder') return 700
  if (value === 'lighter') return 300
  if (!/^[+-]?(?:\d+(?:\.\d*)?|\.\d+)$/.test(value)) return null
  const number = roundCssNumber(Number(value))
  return Math.max(1, Math.min(1000, number))
}

function cssStaticPropertyValue(property, rawValue) {
  const value = rawValue.trim()
  const lower = value.toLowerCase()
  const direct = (prop, v) => ({ property: `gea::embedded::ui::Property::${prop}`, value: v })
  const directLength = (prop, length) => {
    if (!length) return null
    if (length.unit === '') return direct(prop, roundCssNumber(length.number))
    if (length.unit === 'px' && length.number === 0) return direct(prop, 0)
    return null
  }
  const directSize = (lengthProp, percentProp, length) => {
    if (!length) return null
    if (length.unit === '%') return direct(percentProp, clampInt16(roundCssNumber(length.number * 10)))
    return directLength(lengthProp, length)
  }
  const numeric = /^[+-]?(?:\d+(?:\.\d*)?|\.\d+)$/
  const length = simpleStaticLength(value)
  switch (property) {
    case 'display': {
      const values = new Map([['block', 0], ['none', 1], ['grid', 2], ['inline-grid', 2], ['flex', 3], ['inline-flex', 3]])
      return values.has(value) ? direct('Display', values.get(value)) : null
    }
    case 'box-sizing':
      if (value === 'content-box') return direct('BoxSizing', 0)
      if (value === 'border-box') return direct('BoxSizing', 1)
      return null
    case 'flex-direction':
      if (value === 'column') return direct('FlexDirection', 0)
      if (value === 'row') return direct('FlexDirection', 1)
      return null
    case 'flex-wrap':
      if (value === 'nowrap') return direct('FlexWrap', 0)
      if (value === 'wrap') return direct('FlexWrap', 1)
      return null
    case 'justify-content':
      return cssAlignKeywordValue(value) !== null ? direct('JustifyContent', cssAlignKeywordValue(value)) : null
    case 'align-items':
      return cssAlignKeywordValue(value) !== null ? direct('AlignItems', cssAlignKeywordValue(value)) : null
    case 'justify-items':
      return cssAlignKeywordValue(value) !== null ? direct('JustifyItems', cssAlignKeywordValue(value)) : null
    case 'align-content':
      return cssAlignKeywordValue(value) !== null ? direct('AlignContent', cssAlignKeywordValue(value)) : null
    case 'align-self':
      if (value === 'auto') return direct('AlignSelf', -1)
      return cssAlignKeywordValue(value) !== null ? direct('AlignSelf', cssAlignKeywordValue(value)) : null
    case 'position':
      if (value === 'static') return direct('Position', 0)
      if (value === 'absolute') return direct('Position', 1)
      if (value === 'relative') return direct('Position', 2)
      return null
    case 'gap':
      return directLength('Gap', length)
    case 'width':
      return directSize('Width', 'WidthPercent', length)
    case 'height':
      return directSize('Height', 'HeightPercent', length)
    case 'min-width':
      return directLength('MinWidth', length)
    case 'min-height':
      return directLength('MinHeight', length)
    case 'max-width':
      return directLength('MaxWidth', length)
    case 'max-height':
      return directLength('MaxHeight', length)
    case 'flex-basis':
      return directLength('FlexBasis', length)
    case 'flex-shrink':
      return numeric.test(value) ? direct('FlexShrink', roundCssNumber(Number(value))) : null
    case 'font-weight': {
      const weight = cssStaticFontWeightValue(value)
      return weight === null ? null : direct('FontWeight', weight)
    }
    case 'padding-top':
      return directLength('PaddingTop', length)
    case 'padding-right':
      return directLength('PaddingRight', length)
    case 'padding-bottom':
      return directLength('PaddingBottom', length)
    case 'padding-left':
      return directLength('PaddingLeft', length)
    case 'margin-top':
      return directLength('MarginTop', length)
    case 'margin-right':
      return directLength('MarginRight', length)
    case 'margin-bottom':
      return directLength('MarginBottom', length)
    case 'margin-left':
      return directLength('MarginLeft', length)
    case 'top':
      return directSize('Top', 'TopPercent', length)
    case 'right':
      return directSize('Right', 'RightPercent', length)
    case 'bottom':
      return directSize('Bottom', 'BottomPercent', length)
    case 'left':
      return directSize('Left', 'LeftPercent', length)
    case 'z-index':
      return numeric.test(value) ? direct('ZIndex', roundCssNumber(Number(value))) : null
    case 'opacity': {
      if (!numeric.test(value)) return null
      let opacity = Number(value)
      if (opacity <= 1) opacity *= 255
      if (opacity < 0) opacity = 0
      if (opacity > 255) opacity = 255
      return direct('Opacity', Math.floor(opacity + 0.5))
    }
    case 'object-fit':
      if (lower === 'fill') return direct('ImageFit', 0)
      if (lower === 'contain') return direct('ImageFit', 1)
      if (lower === 'cover') return direct('ImageFit', 2)
      if (lower === 'none') return direct('ImageFit', 3)
      if (lower === 'scale-down') return direct('ImageFit', 4)
      return null
    case 'text-align':
      if (value === 'left' || value === 'start') return direct('TextAlign', 0)
      if (value === 'center') return direct('TextAlign', 1)
      if (value === 'right' || value === 'end') return direct('TextAlign', 2)
      return null
    case 'text-decoration':
    case 'text-decoration-line':
      if (value === 'none') return direct('TextDecoration', 0)
      if (value === 'underline') return direct('TextDecoration', 1)
      if (value === 'line-through' || value === 'strikethrough') return direct('TextDecoration', 2)
      return null
    case 'text-transform':
      if (lower === 'none') return direct('TextTransform', 0)
      if (lower === 'uppercase') return direct('TextTransform', 1)
      if (lower === 'lowercase') return direct('TextTransform', 2)
      if (lower === 'capitalize') return direct('TextTransform', 3)
      return null
    case 'white-space':
      if (lower === 'normal' || lower === 'pre-line') return direct('WhiteSpace', 0)
      if (lower === 'nowrap' || lower === 'pre') return direct('WhiteSpace', 1)
      return null
    case 'text-overflow':
      if (lower === 'clip') return direct('TextOverflow', 0)
      if (lower === 'ellipsis') return direct('TextOverflow', 1)
      return null
    case 'backface-visibility':
      if (lower === 'visible') return direct('Backface', 0)
      if (lower === 'hidden') return direct('Backface', 1)
      return null
    case 'pointer-events':
      if (lower === 'auto') return direct('PointerEvents', 0)
      if (lower === 'none') return direct('PointerEvents', 1)
      return null
    case 'overflow':
      if (value === 'visible') return direct('Overflow', 0)
      if (value === 'hidden') return direct('Overflow', 1)
      if (value === 'scroll' || value === 'auto') return direct('Overflow', 2)
      return null
    case 'overflow-x':
      if (value === 'visible') return direct('OverflowX', 0)
      if (value === 'hidden') return direct('OverflowX', 1)
      if (value === 'scroll' || value === 'auto') return direct('OverflowX', 2)
      return null
    case 'overflow-y':
      if (value === 'visible') return direct('OverflowY', 0)
      if (value === 'hidden') return direct('OverflowY', 1)
      if (value === 'scroll' || value === 'auto') return direct('OverflowY', 2)
      return null
    case 'flex-grow':
      return numeric.test(value) ? direct('Flex', roundCssNumber(Number(value))) : null
    default:
      return null
  }
}

function cssStaticPropertyValues(property, rawValue) {
  const single = cssStaticPropertyValue(property, rawValue)
  if (single) return [single]

  const value = rawValue.trim()
  const length = simpleStaticLength(value)
  const direct = (prop, v) => ({ property: `gea::embedded::ui::Property::${prop}`, value: v })
  const directLengthValue = (candidate) => {
    if (!candidate) return null
    if (candidate.unit === '') return roundCssNumber(candidate.number)
    if (candidate.unit === 'px' && candidate.number === 0) return 0
    return null
  }
  const boxLength = directLengthValue(length)
  if (boxLength !== null) {
    const border = {border:'Border','border-top':'BorderTop','border-right':'BorderRight','border-bottom':'BorderBottom','border-left':'BorderLeft'}[property]
    if (border) return [direct(`${border}Width`, boxLength), direct(`${border}Relief`, 0), direct(`${border}ColorCurrent`, 1)]
    if (property === 'margin') {
      return [
        direct('MarginTop', boxLength),
        direct('MarginRight', boxLength),
        direct('MarginBottom', boxLength),
        direct('MarginLeft', boxLength),
      ]
    }
    if (property === 'padding') {
      return [
        direct('PaddingTop', boxLength),
        direct('PaddingRight', boxLength),
        direct('PaddingBottom', boxLength),
        direct('PaddingLeft', boxLength),
      ]
    }
    if (property === 'inset') {
      return [
        direct('Top', boxLength),
        direct('Right', boxLength),
        direct('Bottom', boxLength),
        direct('Left', boxLength),
      ]
    }
  }

  if (property === 'place-items') {
    const align = cssAlignKeywordValue(value)
    if (align !== null) {
      return [
        direct('AlignItems', align),
        direct('JustifyItems', align),
        direct('JustifyContent', align),
      ]
    }
  }

  if (property === 'rotate') {
    const parts = value.toLowerCase().split(/\s+/)
    if (parts.length > 1 && (/(deg|rad|turn|grad)$/.test(parts[0]) ||
        (parts.length === 2 && ['x', 'y', 'z'].includes(parts[1])))) parts.push(parts.shift())
    const none = value.toLowerCase() === 'none'
    const angle = none ? 0 : parseStaticAngleTenths(parts.at(-1))
    if (angle === null || ![1, 2, 4].includes(parts.length)) return []
    if (!none && !/(deg|rad|turn)$/i.test(parts.at(-1)) && Number(parts.at(-1)) !== 0) return []
    let axes = [0, 0, 1000000]
    if (parts.length === 2) {
      const axis = ['x', 'y', 'z'].indexOf(parts[0])
      if (axis < 0) return []
      axes = [0, 0, 0]; axes[axis] = 1000000
    } else if (parts.length === 4) {
      const vector = parts.slice(0, 3).map(Number)
      if (!vector.every(Number.isFinite)) return []
      const magnitude = Math.max(...vector.map(Math.abs))
      axes = magnitude ? vector.map(v => roundCssNumber(v / magnitude * 1000000)) : [0, 0, 0]
    }
    return [direct('RotatePresent', none ? 0 : 1), direct('RotateAngle', axes.some(Boolean) ? angle : 0),
      direct('RotateAxisX', axes[0]), direct('RotateAxisY', axes[1]), direct('RotateAxisZ', axes[2])]
  }
  if (property === 'scale') {
    const none = value.toLowerCase() === 'none'
    const parts = none ? ['1'] : value.split(/\s+/)
    if (parts.length < 1 || parts.length > 3) return []
    const factors = parts.map(v => v.endsWith('%') ? Number(v.slice(0, -1)) / 100 : Number(v))
    if (!factors.every(Number.isFinite)) return []
    const scales = factors.map(v => roundCssNumber(Math.max(-32768, Math.min(32767, v * 1000))))
    return [direct('ScalePresent', none ? 0 : 1), direct('ScaleX', scales[0]),
      direct('ScaleY', scales[1] ?? scales[0]), direct('ScaleZ', scales[2] ?? 1000)]
  }

  return []
}

function cssStaticPrimaryFontFamily(rawValue) {
  let text = rawValue.trim()
  if (!text) return null
  const quote = text[0]
  if (quote === '\'' || quote === '"') {
    const end = text.indexOf(quote, 1)
    return end < 0 ? null : text.slice(1, end)
  }
  const comma = text.indexOf(',')
  if (comma >= 0) text = text.slice(0, comma)
  const family = text.trim()
  return family || null
}

function cssStaticFontFamilyValue(property, rawValue) {
  if (property !== 'font-family') return null
  const family = cssStaticPrimaryFontFamily(rawValue)
  return family ? { family } : null
}

function cssStaticLineHeightValue(property, rawValue) {
  if (property !== 'line-height') return null
  const value = rawValue.trim()
  const lower = value.toLowerCase()
  if (lower === 'normal') {
    return { setup: [], kind: 'Normal', spec: zeroStaticLengthSpec() }
  }
  const numberPattern = /^[+-]?(?:\d+(?:\.\d*)?|\.\d+)$/
  if (numberPattern.test(value)) {
    return { setup: [], kind: 'Scalar', spec: { unit: 'Raw', value: cssNumberCode(Number(value)) } }
  }
  const percent = value.match(/^([+-]?(?:\d+(?:\.\d*)?|\.\d+))%$/)
  if (percent) {
    return { setup: [], kind: 'Percent', spec: { unit: 'Percent', value: cssNumberCode(Number(percent[1])) } }
  }
  const parsed = parseStaticCssLengthSpec(value)
  return parsed ? { ...parsed, kind: 'Length' } : null
}

function cssStaticFlexValue(property, rawValue) {
  if (property !== 'flex') return null
  let grow = 0
  let numberIndex = 0
  let basis = zeroStaticLengthSpec()
  let hasBasis = false
  const setup = []
  const words = splitCssWords(rawValue.trim())
  if (words.length === 0) return null
  // The compact static API has no shrink field. Preserve declarations it
  // cannot represent through the full compiler, including keyword semantics.
  if (words.some(word => /^(none|auto|content|max-content|min-content|fit-content)$/i.test(word))) return null
  for (const token of words) {
    const number = Number(token)
    if (Number.isFinite(number) && token.match(/^[+-]?(?:\d+(?:\.\d*)?|\.\d+)$/)) {
      if (number < 0 || numberIndex > 1) return null
      if (numberIndex === 0) grow = roundCssNumber(number)
      else if (number !== 1) return null
      numberIndex++
      continue
    }
    const parsed = parseStaticCssLengthSpec(token)
    if (!parsed) return null
    setup.push(...parsed.setup)
    basis = parsed.spec
    hasBasis = true
  }
  return { setup, grow, basis, hasBasis }
}

function cssStaticBoxShorthandValue(property, rawValue) {
  const targets = {
    padding: ['PaddingTop', 'PaddingRight', 'PaddingBottom', 'PaddingLeft'],
    margin: ['MarginTop', 'MarginRight', 'MarginBottom', 'MarginLeft'],
    inset: ['Top', 'Right', 'Bottom', 'Left'],
  }[property]
  if (!targets) return null
  const words = splitCssWords(rawValue.trim())
  if (words.length < 1 || words.length > 4) return null
  const parsed = words.map(parseStaticCssLengthSpec)
  if (parsed.some((entry) => !entry)) return null
  const specs =
    parsed.length === 1 ? [parsed[0], parsed[0], parsed[0], parsed[0]] :
    parsed.length === 2 ? [parsed[0], parsed[1], parsed[0], parsed[1]] :
    parsed.length === 3 ? [parsed[0], parsed[1], parsed[2], parsed[1]] :
    [parsed[0], parsed[1], parsed[2], parsed[3]]
  return {
    setup: parsed.flatMap((entry) => entry.setup),
    entries: targets.map((target, index) => ({ target, spec: specs[index].spec })),
  }
}

const staticBorderRadiusCorners = new Map([
  ['border-top-left-radius', 'TopLeft'],
  ['border-top-right-radius', 'TopRight'],
  ['border-bottom-right-radius', 'BottomRight'],
  ['border-bottom-left-radius', 'BottomLeft'],
])

function cssStaticBorderRadiusValue(property, rawValue) {
  if (staticBorderRadiusCorners.has(property)) {
    const parsed = parseStaticCssLengthSpec(rawValue)
    return parsed
      ? { setup: parsed.setup, corner: staticBorderRadiusCorners.get(property), spec: parsed.spec }
      : null
  }
  if (property !== 'border-radius') return null
  const axes = splitTopLevelCss(rawValue, '/')
  const parts = splitCssWords(axes.length > 0 ? axes[0] : rawValue)
  if (parts.length < 1 || parts.length > 4) return null
  const parsed = parts.map(parseStaticCssLengthSpec)
  if (parsed.some((entry) => !entry)) return null
  const specs =
    parsed.length === 1 ? [parsed[0], parsed[0], parsed[0], parsed[0]] :
    parsed.length === 2 ? [parsed[0], parsed[1], parsed[0], parsed[1]] :
    parsed.length === 3 ? [parsed[0], parsed[1], parsed[2], parsed[1]] :
    [parsed[0], parsed[1], parsed[2], parsed[3]]
  return {
    setup: parsed.flatMap((entry) => entry.setup),
    specs: specs.map((entry) => entry.spec),
  }
}

function cssStaticBorderValue(property, rawValue) {
  if (property !== 'border') return null
  if (/\b(?:groove|ridge|inset|outset)\b/i.test(rawValue)) return null
  const parts = splitCssWords(rawValue.trim())
  if (parts.length === 0) return null
  const width = parseStaticCssLengthSpec(parts[0])
  if (!width) return null
  let color = null
  for (const part of parts) {
    const lower = part.toLowerCase()
    if (!part.startsWith('#') && !lower.includes('rgb')) continue
    color = parseStaticCssColor(part)
    break
  }
  return color ? { setup: width.setup, width: width.spec, color } : null
}

function cssStaticBorderShorthandValue(property, rawValue) {
  // The compact width/color API cannot carry a shaded border style.
  if (/\b(?:groove|ridge|inset|outset)\b/i.test(rawValue)) return null
  const targets = {
    border: { width: 'BorderWidth', color: 'Border', colorProperty: 'border-color', side: false },
    'border-top': { width: 'BorderTopWidth', color: 'BorderTop', colorProperty: 'border-top-color', side: true },
    'border-right': { width: 'BorderRightWidth', color: 'BorderRight', colorProperty: 'border-right-color', side: true },
    'border-bottom': { width: 'BorderBottomWidth', color: 'BorderBottom', colorProperty: 'border-bottom-color', side: true },
    'border-left': { width: 'BorderLeftWidth', color: 'BorderLeft', colorProperty: 'border-left-color', side: true },
  }[property]
  if (!targets) return null
  const parts = splitCssWords(rawValue.trim())
  if (parts.length === 0) return null
  const width = parseStaticCssLengthSpec(parts[0])
  if (!width) return null
  let color = null
  let colorVar = null
  for (const part of parts) {
    const literal = parseStaticCssColor(part)
    if (literal) {
      color = { target: targets.color, color: literal }
      break
    }
    const variable = cssStaticColorVarValue(targets.colorProperty, part)
    if (variable) {
      colorVar = variable
      break
    }
  }
  if (!targets.side && color && !colorVar) return null
  return { setup: width.setup, widthTarget: targets.width, reliefTarget: targets.width.replace('Width', 'Relief'),
    currentColorTarget: `${targets.color}ColorCurrent`, width: width.spec, color, colorVar }
}

function staticColorCode(color) {
  return `{${color.r}, ${color.g}, ${color.b}, ${color.a}}`
}

function staticColorRefCode(ref) {
  const name = ref.name ? JSON.stringify(ref.name) : 'nullptr'
  return `{${name}, ${staticColorCode(ref.color)}, ${ref.hasFallback ? 'true' : 'false'}}`
}

function staticLinearGradientCode(gradient) {
  return `{${gradient.angleTenths}, ${staticColorCode(gradient.from)}, ${staticColorCode(gradient.mid)}, ${staticColorCode(gradient.to)}, ${gradient.midStopPermille}, ${gradient.toStopPermille}, ${gradient.hasMid ? 'true' : 'false'}}`
}

function staticLinearGradientRefCode(gradient) {
  return `{${gradient.angleTenths}, ${staticColorRefCode(gradient.from)}, ${staticColorRefCode(gradient.mid)}, ${staticColorRefCode(gradient.to)}, ${gradient.midStopPermille}, ${gradient.toStopPermille}, ${gradient.hasMid ? 'true' : 'false'}}`
}

function staticRadialGradientRefCode(gradient) {
  return `{${staticColorRefCode(gradient.from)}, ${staticColorRefCode(gradient.to)}, ${gradient.stopPermille}, ${gradient.cxPermille}, ${gradient.cyPermille}, ${gradient.rxPermille}, ${gradient.ryPermille}, ${gradient.enabled ? 'true' : 'false'}}`
}

function zeroStaticLinearGradient() {
  const transparent = { r: 0, g: 0, b: 0, a: 0 }
  return {
    angleTenths: 1800,
    from: transparent,
    mid: transparent,
    to: transparent,
    midStopPermille: 500,
    toStopPermille: 1000,
    hasMid: false,
  }
}

function zeroStaticLinearGradientRef() {
  const transparentRef = { name: null, hasFallback: true, color: { r: 0, g: 0, b: 0, a: 0 } }
  return {
    angleTenths: 1800,
    from: transparentRef,
    mid: transparentRef,
    to: transparentRef,
    midStopPermille: 500,
    toStopPermille: 1000,
    hasMid: false,
  }
}

function zeroStaticRadialGradientRef() {
  const transparentRef = { name: null, hasFallback: true, color: { r: 0, g: 0, b: 0, a: 0 } }
  return {
    from: transparentRef,
    to: transparentRef,
    stopPermille: 1000,
    cxPermille: 500,
    cyPermille: 500,
    rxPermille: 1000,
    ryPermille: 1000,
    enabled: false,
  }
}

function staticGridLineCode(line) {
  if (!line) return `{false, ${staticLengthSpecCode(zeroStaticLengthSpec())}, {0, 0, 0, 255}}`
  return `{true, ${staticLengthSpecCode(line.width)}, ${staticColorCode(line.color)}}`
}

function staticGridTemplateTrackCode(track) {
  return `{${track.type}, ${track.value}, ${staticLengthSpecCode(track.length)}}`
}

function cssHasDynamicValue(value) {
  const lower = value.toLowerCase()
  return lower.includes('var(') ||
    lower.includes('calc(') ||
    lower.includes('min(') ||
    lower.includes('max(') ||
    lower.includes('clamp(')
}

function staticColorEquals(a, b) {
  return a && b && a.r === b.r && a.g === b.g && a.b === b.b && a.a === b.a
}

function staticLengthSpecEquals(a, b) {
  return a && b && a.unit === b.unit && a.value === b.value
}

function staticLengthSpecIsSafeGridLine(spec) {
  if (!spec || (spec.unit !== 'Raw' && spec.unit !== 'Px')) return false
  const value = Number(spec.value)
  return Number.isFinite(value) && value >= 1
}

function parseStaticGradientStop(rawStop) {
  const words = splitCssWords(rawStop.trim())
  if (words.length === 0) return null
  const color = parseStaticCssColor(words[0])
  if (!color) return null
  let stopPermille = null
  let length = null
  if (words.length > 1) {
    const stop = words[1].trim()
    const percent = stop.match(/^([+-]?(?:\d+(?:\.\d*)?|\.\d+))%$/)
    if (percent) {
      stopPermille = roundCssNumber(Number(percent[1]) * 10)
    } else {
      const parsedLength = parseStaticCssLengthSpec(stop)
      if (parsedLength) length = parsedLength
    }
  }
  return { color, stopPermille, length }
}

function parseStaticGradientStopRef(rawStop) {
  const words = splitCssWords(rawStop.trim())
  if (words.length === 0) return null
  const color = parseStaticCssColorRef(words[0])
  if (!color) return null
  let stopPermille = null
  if (words.length > 1) {
    const stop = words[1].trim()
    const percent = stop.match(/^([+-]?(?:\d+(?:\.\d*)?|\.\d+))%$/)
    if (percent) stopPermille = roundCssNumber(Number(percent[1]) * 10)
  }
  return { color, stopPermille }
}

function parseStaticGradientHeader(parts) {
  if (parts.length < 2) return null
  const first = parts[0].trim()
  const lower = first.toLowerCase()
  if (first.includes('deg') || first.includes('turn') || first.includes('rad')) {
    const angle = parseStaticAngleTenths(first)
    return angle === null ? null : { angleTenths: angle, colorStart: 1 }
  }
  if (lower.startsWith('to ')) {
    let angleTenths = 1800
    if (lower.includes('right')) angleTenths = 900
    else if (lower.includes('left')) angleTenths = 2700
    else if (lower.includes('top')) angleTenths = 0
    return { angleTenths, colorStart: 1 }
  }
  return { angleTenths: 1800, colorStart: 0 }
}

function parseStaticLinearGradientRefLayer(rawLayer) {
  const inner = staticCssFunctionInner(rawLayer.trim(), 'linear-gradient')
  if (inner === null) return null
  const parts = splitTopLevelCss(inner, ',')
  const header = parseStaticGradientHeader(parts)
  if (!header || header.colorStart >= parts.length) return null

  const from = parseStaticGradientStopRef(parts[header.colorStart])
  const to = parseStaticGradientStopRef(parts[parts.length - 1])
  if (!from || !to) return null

  let mid = { color: { name: null, hasFallback: true, color: { r: 0, g: 0, b: 0, a: 0 } }, stopPermille: 500 }
  let hasMid = false
  if (parts.length === header.colorStart + 2) {
    if (from.stopPermille !== null && from.stopPermille > 0 && from.stopPermille < 1000) {
      mid = { color: from.color, stopPermille: from.stopPermille }
      hasMid = true
    }
  } else if (parts.length > header.colorStart + 2) {
    const parsedMid = parseStaticGradientStopRef(parts[header.colorStart + 1])
    if (!parsedMid) return null
    mid = {
      color: parsedMid.color,
      stopPermille: parsedMid.stopPermille === null ? 500 : Math.max(0, Math.min(1000, parsedMid.stopPermille)),
    }
    hasMid = true
  }

  let toStopPermille = to.stopPermille === null ? 1000 : Math.max(0, Math.min(60000, to.stopPermille))
  if (toStopPermille <= 0) toStopPermille = 1
  if (hasMid && toStopPermille <= mid.stopPermille) toStopPermille = mid.stopPermille + 1

  return {
    angleTenths: header.angleTenths,
    from: from.color,
    mid: mid.color,
    to: to.color,
    midStopPermille: mid.stopPermille,
    toStopPermille,
    hasMid,
  }
}

function parseStaticPercentPermille(rawPart, fallback) {
  const length = parseStaticCssLength(rawPart.trim())
  if (!length || length.unit !== 'Percent') return fallback
  return roundCssNumber(length.number * 10)
}

function parseStaticRadialGradientRefLayer(rawLayer) {
  const inner = staticCssFunctionInner(rawLayer.trim(), 'radial-gradient')
  if (inner === null) return null
  const parts = splitTopLevelCss(inner, ',')
  if (parts.length < 2) return null

  let colorStart = 0
  let cxPermille = 500
  let cyPermille = 500
  let rxPermille = 1000
  let ryPermille = 1000
  if (!parseStaticGradientStopRef(parts[0])) {
    colorStart = 1
    const words = splitCssWords(parts[0])
    let atIndex = words.length
    for (let i = 0; i < words.length; i++) {
      if (words[i].toLowerCase() === 'at') {
        atIndex = i
        break
      }
    }
    const sizeWords = []
    for (let i = 0; i < atIndex; i++) {
      const lower = words[i].toLowerCase()
      if (lower === 'circle' || lower === 'ellipse' || lower === 'closest-side' ||
          lower === 'closest-corner' || lower === 'farthest-side' || lower === 'farthest-corner')
        continue
      sizeWords.push(words[i])
    }
    if (sizeWords.length > 0) rxPermille = parseStaticPercentPermille(sizeWords[0], rxPermille)
    if (sizeWords.length > 1) ryPermille = parseStaticPercentPermille(sizeWords[1], ryPermille)
    else if (sizeWords.length > 0) ryPermille = rxPermille

    if (atIndex < words.length) {
      const centerWords = words.slice(atIndex + 1)
      if (centerWords.length > 0) {
        const x = parseStaticOriginPart(centerWords[0], cxPermille)
        if (x === null) return null
        cxPermille = x
      }
      if (centerWords.length > 1) {
        const y = parseStaticOriginPart(centerWords[1], cyPermille)
        if (y === null) return null
        cyPermille = y
      }
    }
  }
  if (colorStart + 1 >= parts.length) return null
  const from = parseStaticGradientStopRef(parts[colorStart])
  const to = parseStaticGradientStopRef(parts[parts.length - 1])
  if (!from || !to) return null
  let stopPermille = to.stopPermille === null ? 1000 : Math.max(0, Math.min(1000, to.stopPermille))
  if (stopPermille <= 0) stopPermille = 1
  return {
    from: from.color,
    to: to.color,
    stopPermille,
    cxPermille,
    cyPermille,
    rxPermille,
    ryPermille,
    enabled: true,
  }
}

function parseStaticLinearGradientLayer(rawLayer) {
  const inner = staticCssFunctionInner(rawLayer.trim(), 'linear-gradient')
  if (inner === null) return null
  const parts = splitTopLevelCss(inner, ',')
  const header = parseStaticGradientHeader(parts)
  if (!header || header.colorStart >= parts.length) return null

  const from = parseStaticGradientStop(parts[header.colorStart])
  const to = parseStaticGradientStop(parts[parts.length - 1])
  if (!from || !to) return null

  let mid = { color: { r: 0, g: 0, b: 0, a: 0 }, stopPermille: 500 }
  let hasMid = false
  if (parts.length === header.colorStart + 2) {
    if (from.stopPermille !== null && from.stopPermille > 0 && from.stopPermille < 1000) {
      mid = { color: from.color, stopPermille: from.stopPermille }
      hasMid = true
    }
  } else if (parts.length > header.colorStart + 2) {
    const parsedMid = parseStaticGradientStop(parts[header.colorStart + 1])
    if (!parsedMid) return null
    mid = {
      color: parsedMid.color,
      stopPermille: parsedMid.stopPermille === null ? 500 : Math.max(0, Math.min(1000, parsedMid.stopPermille)),
    }
    hasMid = true
  }

  let toStopPermille = to.stopPermille === null ? 1000 : Math.max(0, Math.min(60000, to.stopPermille))
  if (toStopPermille <= 0) toStopPermille = 1
  if (hasMid && toStopPermille <= mid.stopPermille) toStopPermille = mid.stopPermille + 1

  return {
    angleTenths: header.angleTenths,
    from: from.color,
    mid: mid.color,
    to: to.color,
    midStopPermille: mid.stopPermille,
    toStopPermille,
    hasMid,
  }
}

function staticGradientLineIsVertical(angleTenths) {
  const angleRadians = angleTenths * Math.PI / 1800
  const dx = Math.sin(angleRadians)
  const dy = -Math.cos(angleRadians)
  return Math.abs(dx) >= Math.abs(dy)
}

function parseStaticGradientLineLayer(rawLayer) {
  const inner = staticCssFunctionInner(rawLayer.trim(), 'linear-gradient')
  if (inner === null) return { matched: false, line: null, setup: [] }
  const parts = splitTopLevelCss(inner, ',')
  const header = parseStaticGradientHeader(parts)
  if (!header || header.colorStart + 1 >= parts.length) return { matched: false, line: null, setup: [] }

  const first = parseStaticGradientStop(parts[header.colorStart])
  const second = parseStaticGradientStop(parts[header.colorStart + 1])
  if (!first || !second || !first.length || second.color.a !== 0) return { matched: false, line: null, setup: [] }
  if (first.color.a === 0 || !staticLengthSpecIsSafeGridLine(first.length.spec))
    return { matched: true, line: null, setup: [] }
  if (second.length && !staticLengthSpecEquals(first.length.spec, second.length.spec))
    return { matched: true, line: null, setup: [] }

  const axis = staticGradientLineIsVertical(header.angleTenths) ? 'x' : 'y'
  return {
    matched: true,
    setup: first.length.setup,
    line: { axis, width: first.length.spec, color: first.color },
  }
}

function cssStaticBackgroundValue(property, rawValue) {
  if (property !== 'background' && property !== 'background-image') return null
  if (cssHasDynamicValue(rawValue)) return null
  const layers = splitTopLevelCss(rawValue, ',')
  if (layers.length === 0) return null
  // The gradient registration API has no base-color field. Keep combined
  // color/image shorthands on the complete native declaration parser.
  if (layers.some(layer => splitCssWords(layer).some(word => parseStaticCssColor(word) || /^(?:none|border-box|padding-box|content-box)$/i.test(word)))) return null

  const out = {
    setup: [],
    gradient: null,
    overlayGradient: zeroStaticLinearGradient(),
    hasOverlayGradient: false,
    gridX: null,
    gridY: null,
    gridOrder: [],
  }

  for (const layer of layers) {
    const line = parseStaticGradientLineLayer(layer)
    if (line.matched) {
      if (!line.line) return null
      out.setup.push(...line.setup)
      if (line.line.axis === 'x') out.gridX = line.line
      else out.gridY = line.line
      out.gridOrder.push(line.line.axis)
      continue
    }

    const gradient = parseStaticLinearGradientLayer(layer)
    if (gradient) {
      if (out.gradient) {
        out.overlayGradient = out.gradient
        out.hasOverlayGradient = true
      }
      out.gradient = gradient
      continue
    }
    if (layer.toLowerCase().includes('var(')) return null
  }

  if (!out.gradient) return null
  if (layers.length !== 1 + Number(out.hasOverlayGradient) + Number(Boolean(out.radialGradient?.enabled)) + Number(Boolean(out.gridX)) + Number(Boolean(out.gridY))) return null
  if (out.gridX && out.gridY && !staticColorEquals(out.gridX.color, out.gridY.color) && out.gridOrder.join('') !== 'xy') return null
  return out
}

function colorRefUsesVar(ref) {
  return Boolean(ref && ref.name)
}

function linearGradientRefUsesVar(gradient) {
  return colorRefUsesVar(gradient.from) ||
    colorRefUsesVar(gradient.mid) ||
    colorRefUsesVar(gradient.to)
}

function cssStaticBackgroundFullValue(property, rawValue) {
  if (property !== 'background' && property !== 'background-image') return null
  const lower = rawValue.toLowerCase()
  if (lower.includes('calc(') || lower.includes('min(') || lower.includes('max(') || lower.includes('clamp('))
    return null
  const layers = splitTopLevelCss(rawValue, ',')
  if (layers.length === 0) return null
  // The gradient registration API has no base-color field. Keep combined
  // color/image shorthands on the complete native declaration parser.
  if (layers.some(layer => splitCssWords(layer).some(word => parseStaticCssColor(word) || /^(?:none|border-box|padding-box|content-box)$/i.test(word)))) return null

  const out = {
    setup: [],
    gradient: null,
    overlayGradient: zeroStaticLinearGradientRef(),
    hasOverlayGradient: false,
    radialGradient: zeroStaticRadialGradientRef(),
    gridX: null,
    gridY: null,
    gridOrder: [],
    needsFullApi: false,
  }

  for (const layer of layers) {
    const line = parseStaticGradientLineLayer(layer)
    if (line.matched) {
      if (!line.line) return null
      out.setup.push(...line.setup)
      if (line.line.axis === 'x') out.gridX = line.line
      else out.gridY = line.line
      out.gridOrder.push(line.line.axis)
      continue
    }

    const radialGradient = parseStaticRadialGradientRefLayer(layer)
    if (radialGradient) {
      out.radialGradient = radialGradient
      out.needsFullApi = true
      continue
    }

    const gradient = parseStaticLinearGradientRefLayer(layer)
    if (gradient) {
      if (out.gradient) {
        out.overlayGradient = out.gradient
        out.hasOverlayGradient = true
      }
      out.gradient = gradient
      if (linearGradientRefUsesVar(gradient)) out.needsFullApi = true
      continue
    }

    if (layer.toLowerCase().includes('var(')) return null
  }

  if (!out.gradient) return null
  if (layers.length !== 1 + Number(out.hasOverlayGradient) + Number(Boolean(out.radialGradient?.enabled)) + Number(Boolean(out.gridX)) + Number(Boolean(out.gridY))) return null
  if (out.gridX && out.gridY && !staticColorEquals(out.gridX.color, out.gridY.color) && out.gridOrder.join('') !== 'xy') return null
  return out.needsFullApi ? out : null
}

function cssStaticBackgroundSizeValue(property, rawValue) {
  if (property !== 'background-size') return null
  if (cssHasDynamicValue(rawValue)) return null
  const layers = splitTopLevelCss(rawValue, ',')
  if (layers.length !== 1) return null
  const parts = splitCssWords(layers[0])
  if (parts.length === 0 || parts.length > 2) return null
  const parseSize = value => value.toLowerCase() === 'auto'
    ? { setup: [], spec: { unit: 'Auto', value: '0' } }
    : parseStaticCssLengthSpec(value)
  const x = parseSize(parts[0])
  if (!x) return null
  const y = parseSize(parts.length > 1 ? parts[1] : 'auto')
  if (!y) return null
  return { setup: [...x.setup, ...(y === x ? [] : y.setup)], stepX: x.spec, stepY: y.spec }
}

function parseStaticGridTemplateTrack(rawToken) {
  const token = rawToken.trim()
  const lower = token.toLowerCase()
  if (!token) return null
  if (lower === 'auto') return { type: 0, value: 0, length: zeroStaticLengthSpec() }
  if (lower.startsWith('minmax(')) {
    const inner = staticCssFunctionInner(token, 'minmax')
    if (inner === null) return null
    const parts = splitTopLevelCss(inner, ',')
    if (parts.length === 0) return null
    return parseStaticGridTemplateTrack(parts.length > 1 ? parts[1] : parts[0])
  }
  if (lower.endsWith('fr')) {
    const number = Number(lower.slice(0, -2).trim())
    if (!Number.isFinite(number)) return null
    return { type: 2, value: Math.max(1, roundCssNumber(number)), length: zeroStaticLengthSpec() }
  }
  const length = parseStaticCssLengthSpec(token)
  if (!length) return null
  return { type: 1, value: 0, length: length.spec, setup: length.setup }
}

function cssStaticGridTemplateValue(property, rawValue) {
  let target = null
  if (property === 'grid-template-columns') target = 'Columns'
  else if (property === 'grid-template-rows') target = 'Rows'
  const lowerValue = rawValue.toLowerCase()
  if (!target || lowerValue.includes('var(') || lowerValue.includes('calc(') || lowerValue.includes('clamp('))
    return null

  const tracks = []
  const setup = []
  for (const token of splitCssWords(rawValue.trim())) {
    const lower = token.toLowerCase()
    if (lower.startsWith('repeat(')) {
      const inner = staticCssFunctionInner(token, 'repeat')
      if (inner === null) return null
      const parts = splitTopLevelCss(inner, ',')
      if (parts.length < 2) return null
      let repeatCount = roundCssNumber(Number(parts[0].trim()))
      if (!Number.isFinite(repeatCount)) return null
      repeatCount = Math.max(0, Math.min(8, repeatCount))
      const track = parseStaticGridTemplateTrack(parts[1])
      if (!track) return null
      if (track.setup) setup.push(...track.setup)
      for (let i = 0; i < repeatCount && tracks.length < 8; i++) tracks.push(track)
      continue
    }
    const track = parseStaticGridTemplateTrack(token)
    if (!track) return null
    if (track.setup) setup.push(...track.setup)
    if (tracks.length < 8) tracks.push(track)
  }
  return { target, tracks, setup }
}

function parseStaticTimeMs(rawToken) {
  const match = rawToken.trim().toLowerCase().match(/^([+-]?(?:\d+(?:\.\d*)?|\.\d+))(ms|s)$/)
  if (!match) return null
  const amount = Number(match[1])
  if (!Number.isFinite(amount)) return null
  const ms = match[2] === 's' ? amount * 1000 : amount
  return Math.max(0, roundCssNumber(ms))
}

function parseStaticAnimationEasing(rawToken) {
  const token = rawToken.trim()
  const lower = token.toLowerCase()
  const keywords = new Map([
    ['linear', 'Linear'],
    ['ease', 'Ease'],
    ['ease-in', 'EaseIn'],
    ['ease-out', 'EaseOut'],
    ['ease-in-out', 'EaseInOut'],
  ])
  if (keywords.has(lower)) {
    return { kind: keywords.get(lower), x1: 0, y1: 0, x2: 1, y2: 1, steps: 1 }
  }
  const cubic = staticCssFunctionInner(lower, 'cubic-bezier')
  if (cubic !== null) {
    const parts = splitTopLevelCss(cubic, ',').map((part) => Number(part.trim()))
    if (parts.length !== 4 || parts.some((part) => !Number.isFinite(part))) return null
    return { kind: 'CubicBezier', x1: parts[0], y1: parts[1], x2: parts[2], y2: parts[3], steps: 1 }
  }
  const steps = staticCssFunctionInner(lower, 'steps')
  if (steps !== null) {
    const count = Number.parseInt(steps.trim(), 10)
    return { kind: 'Steps', x1: 0, y1: 0, x2: 1, y2: 1, steps: count > 0 ? count : 1 }
  }
  return null
}

function staticAnimationEasingCode(easing) {
  return `{gea::embedded::ui::StaticStyleAnimationEasingKind::${easing.kind}, ${cssNumberCode(easing.x1)}, ${cssNumberCode(easing.y1)}, ${cssNumberCode(easing.x2)}, ${cssNumberCode(easing.y2)}, ${easing.steps}}`
}

function cssStaticAnimationValue(property, rawValue) {
  if (property !== 'animation') return null
  const spec = {
    name: null,
    durationMs: 0,
    delayMs: 0,
    iterations: 1,
    direction: 'Normal',
    fill: 'None',
    easing: { kind: 'Ease', x1: 0, y1: 0, x2: 1, y2: 1, steps: 1 },
  }
  let sawDuration = false
  let sawName = false
  for (const rawToken of splitCssWords(rawValue)) {
    const token = rawToken.trim()
    const lower = token.toLowerCase()
    const timeMs = parseStaticTimeMs(lower)
    if (timeMs !== null) {
      if (!sawDuration) {
        spec.durationMs = timeMs
        sawDuration = true
      } else {
        spec.delayMs = timeMs
      }
      continue
    }
    if (lower === 'infinite') {
      spec.iterations = -1
      continue
    }
    if (/^\d+$/.test(lower)) {
      spec.iterations = Number.parseInt(lower, 10)
      continue
    }
    if (lower === 'reverse') {
      spec.direction = 'Reverse'
      continue
    }
    if (lower === 'alternate') {
      spec.direction = 'Alternate'
      continue
    }
    if (lower === 'alternate-reverse') {
      spec.direction = 'AlternateReverse'
      continue
    }
    if (lower === 'forwards') {
      spec.fill = 'Forwards'
      continue
    }
    if (lower === 'backwards') {
      spec.fill = 'Backwards'
      continue
    }
    if (lower === 'both') {
      spec.fill = 'Both'
      continue
    }
    const easing = parseStaticAnimationEasing(token)
    if (easing) {
      spec.easing = easing
      continue
    }
    if (lower === 'normal' || lower === 'running' || lower === 'none') continue
    if (!sawName) {
      spec.name = token
      sawName = true
    }
  }
  return spec.name && spec.durationMs > 0 ? spec : null
}

function cssStaticCustomLengthValue(property, rawValue) {
  if (!property.startsWith('--')) return null
  const parsed = parseStaticCssLengthSpec(rawValue)
  return parsed ? { name: property, ...parsed } : null
}

function cssStaticCustomColorValue(property, rawValue) {
  if (!property.startsWith('--')) return null
  const color = parseStaticCssColor(rawValue)
  return color ? { name: property, color } : null
}

function cssStaticLengthPropertyTarget(property) {
  const map = new Map([
    ['gap', 'Gap'],
    ['width', 'Width'],
    ['height', 'Height'],
    ['min-width', 'MinWidth'],
    ['min-height', 'MinHeight'],
    ['max-width', 'MaxWidth'],
    ['max-height', 'MaxHeight'],
    ['flex-basis', 'FlexBasis'],
    ['padding-top', 'PaddingTop'],
    ['padding-right', 'PaddingRight'],
    ['padding-bottom', 'PaddingBottom'],
    ['padding-left', 'PaddingLeft'],
    ['margin-top', 'MarginTop'],
    ['margin-right', 'MarginRight'],
    ['margin-bottom', 'MarginBottom'],
    ['margin-left', 'MarginLeft'],
    ['border-width', 'BorderWidth'],
    ['border-top-width', 'BorderTopWidth'],
    ['border-right-width', 'BorderRightWidth'],
    ['border-bottom-width', 'BorderBottomWidth'],
    ['border-left-width', 'BorderLeftWidth'],
    ['font-size', 'FontSize'],
    ['perspective', 'Perspective'],
    ['top', 'Top'],
    ['right', 'Right'],
    ['bottom', 'Bottom'],
    ['left', 'Left'],
  ])
  return map.get(property) ?? null
}

function cssStaticLengthValue(property, rawValue) {
  const target = cssStaticLengthPropertyTarget(property)
  if (!target) return null
  if (rawValue.trim().toLowerCase() === 'auto' && (target === 'Width' || target === 'Height')) {
    return { target, length: { unit: 'Auto', number: 0 } }
  }
  const length = parseStaticCssLength(rawValue)
  if (!length) return null
  return { target, length }
}

function cssStaticLengthExpressionValue(property, rawValue) {
  const target = cssStaticLengthPropertyTarget(property)
  if (!target) return null
  const parsed = parseStaticCssLengthSpec(rawValue)
  if (!parsed || parsed.spec.unit !== 'Expression') return null
  return { target, ...parsed }
}

function cssStaticMaskImageValue(property, rawValue) {
  if (property !== 'mask-image' && property !== '-webkit-mask-image') return null
  const value = rawValue.trim()
  const lower = value.toLowerCase()
  if (!value || lower === 'none') return { setup: [], spec: zeroStaticLengthSpec() }
  if (!lower.includes('linear-gradient') || !lower.includes('to right') || !lower.includes('transparent'))
    return null
  const match = value.match(/calc\(\s*100%\s*-\s*([^)]+?)\s*\)/i)
  if (!match) return null
  const parsed = parseStaticCssLengthSpec(match[1])
  if (!parsed) return null
  return parsed
}

function cssStaticBoxShadowNoneValue(property, rawValue) {
  if (property !== 'box-shadow') return null
  const value = rawValue.trim()
  if (!value || value.toLowerCase() === 'none') return {}
  for (const layer of splitTopLevelCss(value, ',')) {
    for (const token of splitCssWords(layer)) {
      if (token.trim().toLowerCase() === 'inset') return null
    }
  }
  return {}
}

function parseStaticOriginPart(rawPart, fallback) {
  const part = rawPart.trim().toLowerCase()
  if (!part) return fallback
  if (part === 'left' || part === 'top') return 0
  if (part === 'center') return 500
  if (part === 'right' || part === 'bottom') return 1000
  const length = parseStaticCssLength(part)
  if (!length || length.unit !== 'Percent') return null
  return roundCssNumber(length.number * 10)
}

function cssStaticOriginValue(property, rawValue) {
  let target = null
  if (property === 'transform-origin') target = 'TransformOrigin'
  else if (property === 'perspective-origin') target = 'PerspectiveOrigin'
  if (!target) return null
  const parts = rawValue.trim().split(/\s+/).filter(Boolean)
  if (parts.length > 2) return null
  const x = parseStaticOriginPart(parts[0] ?? '', 500)
  const y = parseStaticOriginPart(parts[1] ?? '', 500)
  if (x === null || y === null) return null
  return { target, x, y }
}

const staticTransformFlags = {
  rotateX: 1 << 0,
  rotateY: 1 << 1,
  rotateZ: 1 << 2,
  translateX: 1 << 3,
  translateY: 1 << 4,
  translateZ: 1 << 5,
  scaleX: 1 << 8,
  scaleY: 1 << 9,
  scaleZ: 1 << 13,
}

function parseStaticAngleTenths(rawValue) {
  const value = rawValue.trim()
  const match = value.match(/^([+-]?(?:\d+(?:\.\d*)?|\.\d+))(deg|rad|turn)?$/i)
  if (!match) return null
  let degrees = Number(match[1])
  if (!Number.isFinite(degrees)) return null
  const unit = (match[2] || 'deg').toLowerCase()
  if (unit === 'rad') degrees = degrees * 180 / Math.PI
  else if (unit === 'turn') degrees *= 360
  return roundCssNumber(degrees * 10)
}

function parseStaticScalePermille(rawValue) {
  const value = Number(rawValue.trim())
  if (!Number.isFinite(value)) return null
  return roundCssNumber(value * 1000)
}

function multiplyStaticScalePermille(left, right) {
  return clampInt16(roundCssNumber((left * right) / 1000))
}

function parseStaticTransformValue(rawValue) {
  // Multiple rotation functions need ordered composition. Use the shared C++
  // CSS compiler rather than independently decomposing a matrix in JS.
  if ((rawValue.match(/rotate(?:x|y|z|3d)?\s*\(/gi) || []).length > 1) return null
  const value = rawValue.trim()
  const out = {
    setup: [],
    flags: 0,
    rotateX: 0,
    rotateY: 0,
    rotateZ: 0,
    translateX: zeroStaticLengthSpec(),
    translateY: zeroStaticLengthSpec(),
    translateZ: zeroStaticLengthSpec(),
    scaleX: 1000,
    scaleY: 1000,
    scaleZ: 1000,
  }
  let sawTransform = false
  let i = 0
  while (i < value.length) {
    while (i < value.length && /\s/.test(value[i])) i++
    if (i >= value.length) break
    const nameStart = i
    while (i < value.length && /[A-Za-z3]/.test(value[i])) i++
    if (i === nameStart) return null
    while (i < value.length && /\s/.test(value[i])) i++
    if (i >= value.length || value[i] !== '(') return null
    const name = value.slice(nameStart, i).toLowerCase()
    const argStart = ++i
    let depth = 1
    while (i < value.length && depth > 0) {
      if (value[i] === '(') depth++
      else if (value[i] === ')') depth--
      i++
    }
    if (depth !== 0) return null
    const arg = value.slice(argStart, i - 1)
    const args = splitTopLevelCss(arg, ',')
    const axes = name === 'translatex' ? 1 : name === 'translatey' ? 2 : name === 'translatez' ? 4 :
      (name === 'translate' || name === 'translate3d') ? (1 << Math.min(3, args.length)) - 1 : 0
    out.flags = (out.flags & ~(axes << 10)) | ((out.flags & 7) ? 0 : (axes << 10))
    sawTransform = true

    const setLength = (field, flag, raw) => {
      const parsed = parseStaticCssLengthSpec(raw)
      if (!parsed) return false
      out.setup.push(...parsed.setup)
      out[field] = parsed.spec
      out.flags |= flag
      return true
    }

    if (name === 'rotate' || name === 'rotatez') {
      const angle = parseStaticAngleTenths(arg)
      if (angle === null) return null
      out.rotateZ = angle
      out.flags |= staticTransformFlags.rotateZ
    } else if (name === 'rotatex') {
      const angle = parseStaticAngleTenths(arg)
      if (angle === null) return null
      out.rotateX = angle
      out.flags |= staticTransformFlags.rotateX
    } else if (name === 'rotatey') {
      const angle = parseStaticAngleTenths(arg)
      if (angle === null) return null
      out.rotateY = angle
      out.flags |= staticTransformFlags.rotateY
    } else if (name === 'translatex') {
      if (!setLength('translateX', staticTransformFlags.translateX, arg)) return null
    } else if (name === 'translatey') {
      if (!setLength('translateY', staticTransformFlags.translateY, arg)) return null
    } else if (name === 'translatez') {
      if (!setLength('translateZ', staticTransformFlags.translateZ, arg)) return null
    } else if (name === 'translate' || name === 'translate3d') {
      if (args.length < 1) return null
      if (!setLength('translateX', staticTransformFlags.translateX, args[0])) return null
      if (args.length > 1 && !setLength('translateY', staticTransformFlags.translateY, args[1])) return null
      if (args.length > 2 && !setLength('translateZ', staticTransformFlags.translateZ, args[2])) return null
    } else if (name === 'scale') {
      const sx = parseStaticScalePermille(args[0] ?? arg)
      const sy = parseStaticScalePermille(args[1] ?? args[0] ?? arg)
      if (sx === null || sy === null) return null
      out.scaleX = multiplyStaticScalePermille(out.scaleX, sx)
      out.scaleY = multiplyStaticScalePermille(out.scaleY, sy)
      out.flags |= staticTransformFlags.scaleX | staticTransformFlags.scaleY
    } else if (name === 'scale3d') {
      if (args.length !== 3) return null
      const sx = parseStaticScalePermille(args[0])
      const sy = parseStaticScalePermille(args[1])
      const sz = parseStaticScalePermille(args[2])
      if (sx === null || sy === null || sz === null) return null
      out.scaleX = multiplyStaticScalePermille(out.scaleX, sx)
      out.scaleY = multiplyStaticScalePermille(out.scaleY, sy)
      out.scaleZ = multiplyStaticScalePermille(out.scaleZ, sz)
      out.flags |= staticTransformFlags.scaleX | staticTransformFlags.scaleY | staticTransformFlags.scaleZ
    } else if (name === 'scalex') {
      const scale = parseStaticScalePermille(arg)
      if (scale === null) return null
      out.scaleX = multiplyStaticScalePermille(out.scaleX, scale)
      out.flags |= staticTransformFlags.scaleX
    } else if (name === 'scaley') {
      const scale = parseStaticScalePermille(arg)
      if (scale === null) return null
      out.scaleY = multiplyStaticScalePermille(out.scaleY, scale)
      out.flags |= staticTransformFlags.scaleY
    } else if (name === 'scalez') {
      const scale = parseStaticScalePermille(arg)
      if (scale === null) return null
      out.scaleZ = multiplyStaticScalePermille(out.scaleZ, scale)
      out.flags |= staticTransformFlags.scaleZ
    } else {
      return null
    }
  }
  return sawTransform ? out : null
}

function cssStaticTransformValue(property, rawValue) {
  if (property !== 'transform') return null
  return parseStaticTransformValue(rawValue)
}

function staticTransformArgs(transform, mediaArg = null) {
  const fields = `${transform.flags}, ${transform.rotateX}, ${transform.rotateY}, ${transform.rotateZ}, ${staticLengthSpecCode(transform.translateX)}, ${staticLengthSpecCode(transform.translateY)}, ${staticLengthSpecCode(transform.translateZ)}, ${transform.scaleX}, ${transform.scaleY}`
  if (mediaArg === null) return `${fields}, ${transform.scaleZ}`
  return `${fields}${mediaArg || ', nullptr'}, ${transform.scaleZ}`
}

const staticAnimatableLengthTargets = new Set(['Width', 'Height', 'Top', 'Left'])
const staticAnimatableColorTargets = new Set(['Color', 'Background', 'BackgroundColor'])
const staticAnimatableDirectProperties = new Set([
  'gea::embedded::ui::Property::Opacity',
  'gea::embedded::ui::Property::RotatePresent',
  'gea::embedded::ui::Property::RotateAngle',
  'gea::embedded::ui::Property::RotateAxisX',
  'gea::embedded::ui::Property::RotateAxisY',
  'gea::embedded::ui::Property::RotateAxisZ',
  'gea::embedded::ui::Property::ScalePresent',
  'gea::embedded::ui::Property::ScaleX',
  'gea::embedded::ui::Property::ScaleY',
  'gea::embedded::ui::Property::ScaleZ',
  'gea::embedded::ui::Property::TransformRotate',
  'gea::embedded::ui::Property::TransformScaleX',
  'gea::embedded::ui::Property::TransformScaleY',
  'gea::embedded::ui::Property::TransformScaleZ',
])

function cssStaticFilterBlurValue(property, rawValue) {
  if (property !== 'filter') return null
  const value = rawValue.trim()
  const lower = value.toLowerCase()
  if (!value || lower === 'none') return { setup: [], spec: zeroStaticLengthSpec() }
  const blur = lower.indexOf('blur(')
  if (blur < 0) return { setup: [], spec: zeroStaticLengthSpec() }
  let i = blur + 5
  let depth = 1
  const argStart = i
  while (i < value.length && depth > 0) {
    if (value[i] === '(') depth++
    else if (value[i] === ')') depth--
    i++
  }
  if (depth !== 0 || i <= argStart) return null
  return parseStaticCssLengthSpec(value.slice(argStart, i - 1))
}

function emitStaticCssKeyframeRegistrations(blockName, offset, name, value) {
  if (!useStaticCssRules) return null
  if (cssIgnoredProperty(name)) return []

  if (name === 'transform') {
    const transform = parseStaticTransformValue(value)
    if (transform) {
      return [
        ...transform.setup,
        `__gea_stylesheet.registerStaticTransformKeyframeRule(${JSON.stringify(blockName)}, ${offset}, ${staticTransformArgs(transform)});`,
      ]
    }
  }

  const directProperties = cssStaticPropertyValues(name, value)
  if (directProperties.length > 0 && directProperties.every((direct) => staticAnimatableDirectProperties.has(direct.property))) {
    return directProperties.map((direct) =>
      `__gea_stylesheet.registerStaticPropertyKeyframeRule(${JSON.stringify(blockName)}, ${offset}, ${direct.property}, ${direct.value});`
    )
  }

  const color = cssStaticColorValue(name, value)
  if (color && staticAnimatableColorTargets.has(color.target)) {
    return [
      `__gea_stylesheet.registerStaticColorKeyframeRule(${JSON.stringify(blockName)}, ${offset}, gea::embedded::ui::StaticStyleColorProperty::${color.target}, ${color.color.r}, ${color.color.g}, ${color.color.b}, ${color.color.a});`,
    ]
  }

  const colorVar = cssStaticColorVarValue(name, value)
  if (colorVar && staticAnimatableColorTargets.has(colorVar.target)) {
    return [
      `__gea_stylesheet.registerStaticColorVarKeyframeRule(${JSON.stringify(blockName)}, ${offset}, gea::embedded::ui::StaticStyleColorProperty::${colorVar.target}, ${JSON.stringify(colorVar.name)}, ${colorVar.hasFallback ? 'true' : 'false'}, ${colorVar.color.r}, ${colorVar.color.g}, ${colorVar.color.b}, ${colorVar.color.a});`,
    ]
  }

  const lengthExpression = cssStaticLengthExpressionValue(name, value)
  if (lengthExpression && staticAnimatableLengthTargets.has(lengthExpression.target)) {
    return [
      ...lengthExpression.setup,
      `__gea_stylesheet.registerStaticLengthSpecKeyframeRule(${JSON.stringify(blockName)}, ${offset}, gea::embedded::ui::StaticStyleLengthProperty::${lengthExpression.target}, ${staticLengthSpecCode(lengthExpression.spec)});`,
    ]
  }

  const length = cssStaticLengthValue(name, value)
  if (length && staticAnimatableLengthTargets.has(length.target)) {
    return [
      `__gea_stylesheet.registerStaticLengthKeyframeRule(${JSON.stringify(blockName)}, ${offset}, gea::embedded::ui::StaticStyleLengthProperty::${length.target}, gea::embedded::ui::StaticStyleLengthUnit::${length.length.unit}, ${length.length.number});`,
    ]
  }

  const filterBlur = cssStaticFilterBlurValue(name, value)
  if (filterBlur) {
    return [
      ...filterBlur.setup,
      `__gea_stylesheet.registerStaticFilterBlurKeyframeRule(${JSON.stringify(blockName)}, ${offset}, ${staticLengthSpecCode(filterBlur.spec)});`,
    ]
  }

  return null
}

function emitCssRuleRegistrations(kind, selector, name, value, mediaCondition) {
  const mediaArg = mediaCondition ? `, ${JSON.stringify(mediaCondition)}` : ''
  if (useStaticCssRules) {
    if (cssIgnoredProperty(name)) return []
    const customLength = cssStaticCustomLengthValue(name, value)
    if (customLength) {
      return [
        ...customLength.setup,
        `__gea_stylesheet.registerStaticCustomLengthRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, ${JSON.stringify(customLength.name)}, ${staticLengthSpecCode(customLength.spec)}${mediaArg});`,
      ]
    }
    const customColor = cssStaticCustomColorValue(name, value)
    if (customColor) {
      return [
        `__gea_stylesheet.registerStaticCustomColorRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, ${JSON.stringify(customColor.name)}, ${customColor.color.r}, ${customColor.color.g}, ${customColor.color.b}, ${customColor.color.a}${mediaArg});`,
      ]
    }
    const colorVar = cssStaticColorVarValue(name, value)
    if (colorVar) {
      return [
        `__gea_stylesheet.registerStaticColorVarRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, gea::embedded::ui::StaticStyleColorProperty::${colorVar.target}, ${JSON.stringify(colorVar.name)}, ${colorVar.hasFallback ? 'true' : 'false'}, ${colorVar.color.r}, ${colorVar.color.g}, ${colorVar.color.b}, ${colorVar.color.a}${mediaArg});`,
      ]
    }
    const directProperties = cssStaticPropertyValues(name, value)
    if (directProperties.length > 0) {
      if (directProperties.length === 1) {
        const direct = directProperties[0]
        return [
          `__gea_stylesheet.registerStaticPropertyRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, ${direct.property}, ${direct.value}${mediaArg});`,
        ]
      }
      // Each native compiled property group has four slots.
      const registrations = []
      for (let i = 0; i < directProperties.length; i += 4) {
        const entries = directProperties.slice(i, i + 4)
          .map(direct => `{${direct.property}, ${direct.value}}`).join(', ')
        registrations.push(`__gea_stylesheet.registerStaticPropertyGroupRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, {${entries}}${mediaArg});`)
      }
      return registrations
    }
    const flex = cssStaticFlexValue(name, value)
    if (flex) {
      return [
        ...flex.setup,
        `__gea_stylesheet.registerStaticFlexRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, ${flex.grow}, ${staticLengthSpecCode(flex.basis)}, ${flex.hasBasis ? 'true' : 'false'}${mediaArg});`,
      ]
    }
    const boxShorthand = cssStaticBoxShorthandValue(name, value)
    if (boxShorthand) {
      return [
        ...boxShorthand.setup,
        ...boxShorthand.entries.map((entry) =>
          `__gea_stylesheet.registerStaticLengthSpecRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, gea::embedded::ui::StaticStyleLengthProperty::${entry.target}, ${staticLengthSpecCode(entry.spec)}${mediaArg});`
        ),
      ]
    }
    const fontFamily = cssStaticFontFamilyValue(name, value)
    if (fontFamily) {
      return [
        `__gea_stylesheet.registerStaticFontFamilyRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, ${JSON.stringify(fontFamily.family)}${mediaArg});`,
      ]
    }
    const lineHeight = cssStaticLineHeightValue(name, value)
    if (lineHeight) {
      return [
        ...lineHeight.setup,
        `__gea_stylesheet.registerStaticLineHeightRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, gea::embedded::ui::StaticStyleLineHeightKind::${lineHeight.kind}, ${staticLengthSpecCode(lineHeight.spec)}${mediaArg});`,
      ]
    }
    const borderShorthand = cssStaticBorderShorthandValue(name, value)
    if (borderShorthand) {
      const calls = [
        ...borderShorthand.setup,
        `__gea_stylesheet.registerStaticLengthSpecRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, gea::embedded::ui::StaticStyleLengthProperty::${borderShorthand.widthTarget}, ${staticLengthSpecCode(borderShorthand.width)}${mediaArg});`,
        `__gea_stylesheet.registerStaticPropertyRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, gea::embedded::ui::Property::${borderShorthand.reliefTarget}, 0${mediaArg});`,
      ]
      if (borderShorthand.colorVar) {
        const colorVar = borderShorthand.colorVar
        calls.push(`__gea_stylesheet.registerStaticColorVarRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, gea::embedded::ui::StaticStyleColorProperty::${colorVar.target}, ${JSON.stringify(colorVar.name)}, ${colorVar.hasFallback ? 'true' : 'false'}, ${colorVar.color.r}, ${colorVar.color.g}, ${colorVar.color.b}, ${colorVar.color.a}${mediaArg});`)
      } else if (borderShorthand.color) {
        const color = borderShorthand.color
        calls.push(`__gea_stylesheet.registerStaticColorRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, gea::embedded::ui::StaticStyleColorProperty::${color.target}, ${color.color.r}, ${color.color.g}, ${color.color.b}, ${color.color.a}${mediaArg});`)
      } else {
        calls.push(`__gea_stylesheet.registerStaticPropertyRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, gea::embedded::ui::Property::${borderShorthand.currentColorTarget}, 1${mediaArg});`)
      }
      return calls
    }
    const border = cssStaticBorderValue(name, value)
    if (border) {
      return [
        ...border.setup,
        `__gea_stylesheet.registerStaticBorderRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, ${staticLengthSpecCode(border.width)}, ${border.color.r}, ${border.color.g}, ${border.color.b}, ${border.color.a}${mediaArg});`,
      ]
    }
    const borderRadius = cssStaticBorderRadiusValue(name, value)
    if (borderRadius) {
      if (borderRadius.corner) {
        return [
          ...borderRadius.setup,
          `__gea_stylesheet.registerStaticBorderRadiusCornerRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, gea::embedded::ui::StaticStyleBorderRadiusCorner::${borderRadius.corner}, ${staticLengthSpecCode(borderRadius.spec)}${mediaArg});`,
        ]
      }
      return [
        ...borderRadius.setup,
        `__gea_stylesheet.registerStaticBorderRadiusRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, ${borderRadius.specs.map(staticLengthSpecCode).join(', ')}${mediaArg});`,
      ]
    }
    const filterBlur = cssStaticFilterBlurValue(name, value)
    if (filterBlur) {
      return [
        ...filterBlur.setup,
        `__gea_stylesheet.registerStaticFilterBlurRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, ${staticLengthSpecCode(filterBlur.spec)}${mediaArg});`,
      ]
    }
    const fullBackground = cssStaticBackgroundFullValue(name, value)
    if (fullBackground) {
      return [
        ...fullBackground.setup,
        `__gea_stylesheet.registerStaticBackgroundFullRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, ${staticLinearGradientRefCode(fullBackground.gradient)}, ${staticLinearGradientRefCode(fullBackground.overlayGradient)}, ${fullBackground.hasOverlayGradient ? 'true' : 'false'}, ${staticRadialGradientRefCode(fullBackground.radialGradient)}, ${staticGridLineCode(fullBackground.gridX)}, ${staticGridLineCode(fullBackground.gridY)}${mediaArg || ", nullptr"}, ${name === "background-image" ? "true" : "false"});`,
      ]
    }
    const background = cssStaticBackgroundValue(name, value)
    if (background) {
      return [
        ...background.setup,
        `__gea_stylesheet.registerStaticBackgroundRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, ${staticLinearGradientCode(background.gradient)}, ${staticLinearGradientCode(background.overlayGradient)}, ${background.hasOverlayGradient ? 'true' : 'false'}, ${staticGridLineCode(background.gridX)}, ${staticGridLineCode(background.gridY)}${mediaArg || ", nullptr"}, ${name === "background-image" ? "true" : "false"});`,
      ]
    }
    const backgroundSize = cssStaticBackgroundSizeValue(name, value)
    if (backgroundSize) {
      return [
        ...backgroundSize.setup,
        `__gea_stylesheet.registerStaticBackgroundSizeRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, ${staticLengthSpecCode(backgroundSize.stepX)}, ${staticLengthSpecCode(backgroundSize.stepY)}${mediaArg});`,
      ]
    }
    const gridTemplate = cssStaticGridTemplateValue(name, value)
    if (gridTemplate) {
      return [
        ...gridTemplate.setup,
        `__gea_stylesheet.registerStaticGridTemplateRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, gea::embedded::ui::StaticStyleGridTemplateProperty::${gridTemplate.target}, {${gridTemplate.tracks.map(staticGridTemplateTrackCode).join(', ')}}${mediaArg});`,
      ]
    }
    const maskImage = cssStaticMaskImageValue(name, value)
    if (maskImage) {
      return [
        ...maskImage.setup,
        `__gea_stylesheet.registerStaticLengthSpecRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, gea::embedded::ui::StaticStyleLengthProperty::MaskImage, ${staticLengthSpecCode(maskImage.spec)}${mediaArg});`,
      ]
    }
    const boxShadowNone = cssStaticBoxShadowNoneValue(name, value)
    if (boxShadowNone) {
      return [
        `__gea_stylesheet.registerStaticBoxShadowNoneRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}${mediaArg});`,
      ]
    }
    const animation = cssStaticAnimationValue(name, value)
    if (animation) {
      return [
        `__gea_stylesheet.registerStaticAnimationRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, ${JSON.stringify(animation.name)}, ${animation.durationMs}, ${animation.delayMs}, ${animation.iterations}, gea::embedded::ui::StaticStyleAnimationDirection::${animation.direction}, gea::embedded::ui::StaticStyleAnimationFill::${animation.fill}, ${staticAnimationEasingCode(animation.easing)}${mediaArg});`,
      ]
    }
    const color = cssStaticColorValue(name, value)
    if (color) {
      return [
        `__gea_stylesheet.registerStaticColorRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, gea::embedded::ui::StaticStyleColorProperty::${color.target}, ${color.color.r}, ${color.color.g}, ${color.color.b}, ${color.color.a}${mediaArg});`,
      ]
    }
    const length = cssStaticLengthValue(name, value)
    if (length) {
      return [
        `__gea_stylesheet.registerStaticLengthRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, gea::embedded::ui::StaticStyleLengthProperty::${length.target}, gea::embedded::ui::StaticStyleLengthUnit::${length.length.unit}, ${length.length.number}${mediaArg});`,
      ]
    }
    const lengthExpression = cssStaticLengthExpressionValue(name, value)
    if (lengthExpression) {
      return [
        ...lengthExpression.setup,
        `__gea_stylesheet.registerStaticLengthSpecRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, gea::embedded::ui::StaticStyleLengthProperty::${lengthExpression.target}, ${staticLengthSpecCode(lengthExpression.spec)}${mediaArg});`,
      ]
    }
    const origin = cssStaticOriginValue(name, value)
    if (origin) {
      return [
        `__gea_stylesheet.registerStaticOriginRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, gea::embedded::ui::StaticStyleOriginProperty::${origin.target}, ${origin.x}, ${origin.y}${mediaArg});`,
      ]
    }
    const transform = cssStaticTransformValue(name, value)
    if (transform) {
      return [
        ...transform.setup,
        `__gea_stylesheet.registerStaticTransformRule(${staticSelectorKind(kind)}, ${JSON.stringify(selector)}, ${staticTransformArgs(transform, mediaArg)});`,
      ]
    }
  }
  return [
    `__gea_stylesheet.${cssRegisterCall(kind)}(${JSON.stringify(selector)}, ${JSON.stringify(name)}, ${JSON.stringify(value)}${mediaArg});`,
  ]
}

function emitCssRules(cssText, mediaCondition, calls) {
  calls.push(...staticMediaConditionPlanRegistration(mediaCondition))
  const ruleRegex = /([^{}]+)\{([^{}]*)\}/g
  let ruleMatch
  while ((ruleMatch = ruleRegex.exec(cssText)) !== null) {
    const selectors = ruleMatch[1].split(',').map((selector) => selector.trim()).filter(Boolean)
    // Simple selectors keep their compact legacy registration path.
    // Everything richer (e.g. :root, .a.b, descendants, child selectors,
    // first/last-child, and ::before/::after) goes through the runtime
    // selector matcher.
    const classNames = []
    const activeClassNames = []
    const elementNames = []
    const selectorRules = []
    for (const selector of selectors) {
      const className = selector.match(/^\.([A-Za-z0-9_-]+)$/)?.[1]
      if (className) {
        classNames.push(className)
        continue
      }
      const activeClass = selector.match(/^\.([A-Za-z0-9_-]+):active$/)?.[1]
      if (activeClass) {
        activeClassNames.push(activeClass)
        continue
      }
      const elementName = selector.match(/^([A-Za-z][A-Za-z0-9-]*)$/)?.[1]
      if (elementName) {
        elementNames.push(elementName.toLowerCase())
        continue
      }
      selectorRules.push(selector)
    }
    if (classNames.length === 0 && activeClassNames.length === 0 && elementNames.length === 0 && selectorRules.length === 0) continue
    for (const selector of selectorRules) {
      calls.push(...staticSelectorPlanRegistration(selector))
    }
    const declarations = ruleMatch[2]
      .split(';')
      .map((decl) => decl.trim())
      .filter(Boolean)
    for (const declaration of declarations) {
      const colon = declaration.indexOf(':')
      if (colon <= 0) continue
      const name = declaration.slice(0, colon).trim()
      const value = declaration.slice(colon + 1).trim()
      if (!name || !value) continue
      for (const className of classNames) {
        calls.push(...emitCssRuleRegistrations('class', className, name, value, mediaCondition))
      }
      for (const className of activeClassNames) {
        // `background-color` in a :active rule becomes
        // `active-background-color`, which the framework's CSS parser
        // routes to Property::ActiveBackgroundColor (and auto-sets
        // HasActiveBackground). Other properties (color, opacity) get
        // the same `active-` prefix so they only swap in during a tap.
        const activeName = name.startsWith('active-') ? name : `active-${name}`
        calls.push(...emitCssRuleRegistrations('class', className, activeName, value, mediaCondition))
      }
      for (const elementName of elementNames) {
        calls.push(...emitCssRuleRegistrations('element', elementName, name, value, mediaCondition))
      }
      for (const selector of selectorRules) {
        calls.push(...emitCssRuleRegistrations('selector', selector, name, value, mediaCondition))
      }
    }
  }
}

// Split CSS into its top-level rules and its @media blocks. The rule regex
// can't handle the nested braces inside @media, so we brace-match each block
// out first and hand its body to emitCssRules tagged with the raw condition.
function extractMediaBlocks(css) {
  const media = []
  let base = ''
  let i = 0
  while (i < css.length) {
    const at = css.indexOf('@media', i)
    if (at === -1) {
      base += css.slice(i)
      break
    }
    base += css.slice(i, at)
    const braceOpen = css.indexOf('{', at)
    if (braceOpen === -1) {
      base += css.slice(at)
      break
    }
    const condition = css.slice(at + 6, braceOpen).replace(/\s+/g, ' ').trim()
    let depth = 1
    let j = braceOpen + 1
    for (; j < css.length && depth > 0; j++) {
      if (css[j] === '{') depth++
      else if (css[j] === '}') depth--
    }
    // j points just past the matched closing brace; body excludes it.
    media.push({ condition, body: css.slice(braceOpen + 1, depth === 0 ? j - 1 : j) })
    i = j
  }
  return { base, media }
}

function extractKeyframeBlocks(css) {
  const keyframes = []
  let base = ''
  let i = 0
  while (i < css.length) {
    const at = css.indexOf('@keyframes', i)
    if (at === -1) {
      base += css.slice(i)
      break
    }
    base += css.slice(i, at)
    const braceOpen = css.indexOf('{', at)
    if (braceOpen === -1) {
      base += css.slice(at)
      break
    }
    const name = css.slice(at + '@keyframes'.length, braceOpen).trim()
    let depth = 1
    let j = braceOpen + 1
    for (; j < css.length && depth > 0; j++) {
      if (css[j] === '{') depth++
      else if (css[j] === '}') depth--
    }
    keyframes.push({ name, body: css.slice(braceOpen + 1, depth === 0 ? j - 1 : j) })
    i = j
  }
  return { base, keyframes }
}

function keyframeOffsetPermille(selector) {
  const trimmed = selector.trim()
  if (trimmed === 'from') return 0
  if (trimmed === 'to') return 1000
  const percent = trimmed.match(/^(-?\d+(?:\.\d+)?)%$/)
  if (!percent) return null
  const value = Math.round(Number(percent[1]) * 10)
  return Math.max(0, Math.min(1000, value))
}

function emitCssKeyframes(blocks, calls) {
  for (const block of blocks) {
    if (!block.name) continue
    const frameRegex = /([^{}]+)\{([^{}]*)\}/g
    let frameMatch
    while ((frameMatch = frameRegex.exec(block.body)) !== null) {
      const offsets = splitTopLevelCss(frameMatch[1], ',')
        .map(keyframeOffsetPermille)
        .filter(offset => offset !== null)
      if (offsets.length === 0) continue
      const declarations = frameMatch[2]
        .split(';')
        .map(decl => decl.trim())
        .filter(Boolean)
      for (const declaration of declarations) {
        const colon = declaration.indexOf(':')
        if (colon <= 0) continue
        const name = declaration.slice(0, colon).trim()
        const value = declaration.slice(colon + 1).trim()
        if (!name || !value) continue
        for (const offset of offsets) {
          const staticCalls = emitStaticCssKeyframeRegistrations(block.name, offset, name, value)
          if (staticCalls) {
            calls.push(...staticCalls)
            continue
          }
          calls.push(`__gea_stylesheet.${cssRegisterCall('keyframe')}(${JSON.stringify(block.name)}, ${offset}, ${JSON.stringify(name)}, ${JSON.stringify(value)});`)
        }
      }
    }
  }
}

function splitTopLevelCppArgs(value) {
  const out = []
  let start = 0
  let paren = 0
  let brace = 0
  let inString = false
  let escaped = false
  for (let i = 0; i < value.length; i++) {
    const char = value[i]
    if (inString) {
      if (escaped) escaped = false
      else if (char === '\\') escaped = true
      else if (char === '"') inString = false
      continue
    }
    if (char === '"') {
      inString = true
      continue
    }
    if (char === '(') paren++
    else if (char === ')' && paren > 0) paren--
    else if (char === '{') brace++
    else if (char === '}' && brace > 0) brace--
    else if (char === ',' && paren === 0 && brace === 0) {
      out.push(value.slice(start, i).trim())
      start = i + 1
    }
  }
  out.push(value.slice(start).trim())
  return out.filter(Boolean)
}

function staticCssTapeLengthSpecParts(code) {
  const trimmed = code.trim()
  if (!trimmed.startsWith('{') || !trimmed.endsWith('}')) return null
  const parts = splitTopLevelCppArgs(trimmed.slice(1, -1))
  if (parts.length !== 2) return null
  // The tape is constexpr: a length expression's value is the id its
  // registration returned at run time, so that rule stays a plain call.
  if (!/^[-+]?(\d+\.?\d*|\.\d+)(e[-+]?\d+)?f?$/i.test(parts[1].trim())) return null
  return { unit: parts[0], value: parts[1] }
}

function staticCssTapeEntry(call, stringIndex) {
  if (!useStaticCssTape || !useStaticCssRules) return null
  const match = call.match(/^__gea_stylesheet\.(registerStatic[A-Za-z0-9_]+)\(([\s\S]*)\);$/)
  if (!match) return null
  const method = match[1]
  const args = splitTopLevelCppArgs(match[2])
  const media = (index) => args[index] ?? 'nullptr'
  const selectorKind = (value) => `static_cast<std::uint8_t>(${value})`
  const enumSlot = (value) => `static_cast<std::uint16_t>(${value})`
  const lengthUnit = (value) => `static_cast<std::uint8_t>(${value})`
  const entry = (kind, code) => ({ kind, code, call })

  switch (method) {
    case 'registerStaticPropertyRule':
      if (args.length < 4 || args.length > 5) return null
      return entry('Property', `{${args[3]}, ${stringIndex(args[1])}, ${stringIndex(media(4))}, ${enumSlot(args[2])}, ${selectorKind(args[0])}}`)
    case 'registerStaticLengthRule':
      if (args.length < 5 || args.length > 6) return null
      return entry('Length', `{${args[4]}, ${stringIndex(args[1])}, ${stringIndex(media(5))}, ${enumSlot(args[2])}, ${selectorKind(args[0])}, ${lengthUnit(args[3])}}`)
    case 'registerStaticLengthSpecRule':
      if (args.length < 4 || args.length > 5) return null
      {
        const length = staticCssTapeLengthSpecParts(args[3])
        if (!length) return null
        return entry('LengthSpec', `{${length.value}, ${stringIndex(args[1])}, ${stringIndex(media(4))}, ${enumSlot(args[2])}, ${selectorKind(args[0])}, ${lengthUnit(length.unit)}}`)
      }
    case 'registerStaticColorRule':
      if (args.length < 7 || args.length > 8) return null
      return entry('Color', `{${stringIndex(args[1])}, ${stringIndex(media(7))}, ${enumSlot(args[2])}, ${selectorKind(args[0])}, ${args[3]}, ${args[4]}, ${args[5]}, ${args[6]}}`)
    case 'registerStaticColorVarRule':
      if (args.length < 9 || args.length > 10) return null
      return entry('ColorVar', `{${stringIndex(args[1])}, ${stringIndex(media(9))}, ${stringIndex(args[3])}, ${enumSlot(args[2])}, ${selectorKind(args[0])}, ${args[4]}, ${args[5]}, ${args[6]}, ${args[7]}, ${args[8]}}`)
    case 'registerStaticCustomLengthRule':
      if (args.length < 4 || args.length > 5) return null
      {
        const length = staticCssTapeLengthSpecParts(args[3])
        if (!length) return null
        return entry('CustomLength', `{${length.value}, ${stringIndex(args[1])}, ${stringIndex(media(4))}, ${stringIndex(args[2])}, ${selectorKind(args[0])}, ${lengthUnit(length.unit)}}`)
      }
    case 'registerStaticCustomColorRule':
      if (args.length < 7 || args.length > 8) return null
      return entry('CustomColor', `{${stringIndex(args[1])}, ${stringIndex(media(7))}, ${stringIndex(args[2])}, ${selectorKind(args[0])}, ${args[3]}, ${args[4]}, ${args[5]}, ${args[6]}}`)
    case 'registerStaticBoxShadowNoneRule':
      if (args.length < 2 || args.length > 3) return null
      return entry('BoxShadowNone', `{${stringIndex(args[1])}, ${stringIndex(media(2))}, ${selectorKind(args[0])}}`)
    case 'registerStaticFontFamilyRule':
      if (args.length < 3 || args.length > 4) return null
      return entry('FontFamily', `{${stringIndex(args[1])}, ${stringIndex(media(3))}, ${stringIndex(args[2])}, ${selectorKind(args[0])}}`)
    case 'registerStaticFilterBlurRule':
      if (args.length < 3 || args.length > 4) return null
      {
        const length = staticCssTapeLengthSpecParts(args[2])
        if (!length) return null
        return entry('FilterBlur', `{${length.value}, ${stringIndex(args[1])}, ${stringIndex(media(3))}, ${selectorKind(args[0])}, ${lengthUnit(length.unit)}}`)
      }
    case 'registerStaticLineHeightRule':
      if (args.length < 4 || args.length > 5) return null
      {
        const length = staticCssTapeLengthSpecParts(args[3])
        if (!length) return null
        return entry('LineHeight', `{${length.value}, ${stringIndex(args[1])}, ${stringIndex(media(4))}, ${enumSlot(args[2])}, ${selectorKind(args[0])}, ${lengthUnit(length.unit)}}`)
      }
    default:
      return null
  }
}

function staticCssTapePrelude(stringLiterals) {
  const strings = stringLiterals.map((value) => `\t${value}`).join(',\n')
  return `
const char * const __gea_static_css_tape_strings[] = {
${strings}
};
enum class __GeaStaticCssTapeKind : std::uint8_t {
\tProperty,
\tLength,
\tLengthSpec,
\tColor,
\tColorVar,
\tCustomLength,
\tCustomColor,
\tBoxShadowNone,
\tFontFamily,
\tFilterBlur,
\tLineHeight
};
struct __GeaStaticCssPropertyOp {
\tint value;
\tstd::uint16_t selector;
\tstd::uint16_t media;
\tstd::uint16_t property;
\tstd::uint8_t selectorKind;
};
struct __GeaStaticCssLengthOp {
\tfloat value;
\tstd::uint16_t selector;
\tstd::uint16_t media;
\tstd::uint16_t property;
\tstd::uint8_t selectorKind;
\tstd::uint8_t unit;
};
struct __GeaStaticCssColorOp {
\tstd::uint16_t selector;
\tstd::uint16_t media;
\tstd::uint16_t property;
\tstd::uint8_t selectorKind;
\tstd::uint8_t r;
\tstd::uint8_t g;
\tstd::uint8_t b;
\tstd::uint8_t a;
};
struct __GeaStaticCssColorVarOp {
\tstd::uint16_t selector;
\tstd::uint16_t media;
\tstd::uint16_t name;
\tstd::uint16_t property;
\tstd::uint8_t selectorKind;
\tbool hasFallback;
\tstd::uint8_t r;
\tstd::uint8_t g;
\tstd::uint8_t b;
\tstd::uint8_t a;
};
struct __GeaStaticCssCustomLengthOp {
\tfloat value;
\tstd::uint16_t selector;
\tstd::uint16_t media;
\tstd::uint16_t name;
\tstd::uint8_t selectorKind;
\tstd::uint8_t unit;
};
struct __GeaStaticCssCustomColorOp {
\tstd::uint16_t selector;
\tstd::uint16_t media;
\tstd::uint16_t name;
\tstd::uint8_t selectorKind;
\tstd::uint8_t r;
\tstd::uint8_t g;
\tstd::uint8_t b;
\tstd::uint8_t a;
};
struct __GeaStaticCssSelectorOnlyOp {
\tstd::uint16_t selector;
\tstd::uint16_t media;
\tstd::uint8_t selectorKind;
};
struct __GeaStaticCssNamedOp {
\tstd::uint16_t selector;
\tstd::uint16_t media;
\tstd::uint16_t name;
\tstd::uint8_t selectorKind;
};
struct __GeaStaticCssLengthOnlyOp {
\tfloat value;
\tstd::uint16_t selector;
\tstd::uint16_t media;
\tstd::uint8_t selectorKind;
\tstd::uint8_t unit;
};
struct __GeaStaticCssLineHeightOp {
\tfloat value;
\tstd::uint16_t selector;
\tstd::uint16_t media;
\tstd::uint16_t kind;
\tstd::uint8_t selectorKind;
\tstd::uint8_t unit;
};
auto __gea_css_tape_string = [&](std::uint16_t index) -> const char * {
\treturn __gea_static_css_tape_strings[index];
};
auto __gea_css_tape_selector_kind = [](std::uint8_t kind) {
\treturn static_cast<gea::embedded::ui::StaticStyleSelectorKind>(kind);
};
auto __gea_css_tape_length = [](std::uint8_t unit, float value) {
\treturn gea::embedded::ui::StaticStyleLengthSpec{static_cast<gea::embedded::ui::StaticStyleLengthUnit>(unit), value};
};
// Each tape's lists are static constexpr arrays the call points into: an
// initializer_list argument is a temporary array, which the compiler builds on
// the stack by copying it out of read-only data at every call.
struct __GeaStaticCssTape {
\tstd::size_t orderCount;
\tconst __GeaStaticCssTapeKind *order;
\tconst __GeaStaticCssPropertyOp *propertyOps;
\tconst __GeaStaticCssLengthOp *lengthOps;
\tconst __GeaStaticCssLengthOp *lengthSpecOps;
\tconst __GeaStaticCssColorOp *colorOps;
\tconst __GeaStaticCssColorVarOp *colorVarOps;
\tconst __GeaStaticCssCustomLengthOp *customLengthOps;
\tconst __GeaStaticCssCustomColorOp *customColorOps;
\tconst __GeaStaticCssSelectorOnlyOp *boxShadowNoneOps;
\tconst __GeaStaticCssNamedOp *fontFamilyOps;
\tconst __GeaStaticCssLengthOnlyOp *filterBlurOps;
\tconst __GeaStaticCssLineHeightOp *lineHeightOps;
};
auto __gea_register_static_css_tape = [&](const __GeaStaticCssTape &tape) {
\tconst auto *propertyIt = tape.propertyOps;
\tconst auto *lengthIt = tape.lengthOps;
\tconst auto *lengthSpecIt = tape.lengthSpecOps;
\tconst auto *colorIt = tape.colorOps;
\tconst auto *colorVarIt = tape.colorVarOps;
\tconst auto *customLengthIt = tape.customLengthOps;
\tconst auto *customColorIt = tape.customColorOps;
\tconst auto *boxShadowNoneIt = tape.boxShadowNoneOps;
\tconst auto *fontFamilyIt = tape.fontFamilyOps;
\tconst auto *filterBlurIt = tape.filterBlurOps;
\tconst auto *lineHeightIt = tape.lineHeightOps;
\tfor (std::size_t step = 0; step < tape.orderCount; ++step) {
\t\tconst auto kind = tape.order[step];
\t\tswitch (kind) {
\t\tcase __GeaStaticCssTapeKind::Property: {
\t\t\tconst auto &op = *propertyIt++;
\t\t\t__gea_stylesheet.registerStaticPropertyRule(__gea_css_tape_selector_kind(op.selectorKind), __gea_css_tape_string(op.selector), static_cast<gea::embedded::ui::Property>(op.property), op.value, __gea_css_tape_string(op.media));
\t\t\tbreak;
\t\t}
\t\tcase __GeaStaticCssTapeKind::Length: {
\t\t\tconst auto &op = *lengthIt++;
\t\t\t__gea_stylesheet.registerStaticLengthRule(__gea_css_tape_selector_kind(op.selectorKind), __gea_css_tape_string(op.selector), static_cast<gea::embedded::ui::StaticStyleLengthProperty>(op.property), static_cast<gea::embedded::ui::StaticStyleLengthUnit>(op.unit), op.value, __gea_css_tape_string(op.media));
\t\t\tbreak;
\t\t}
\t\tcase __GeaStaticCssTapeKind::LengthSpec: {
\t\t\tconst auto &op = *lengthSpecIt++;
\t\t\t__gea_stylesheet.registerStaticLengthSpecRule(__gea_css_tape_selector_kind(op.selectorKind), __gea_css_tape_string(op.selector), static_cast<gea::embedded::ui::StaticStyleLengthProperty>(op.property), __gea_css_tape_length(op.unit, op.value), __gea_css_tape_string(op.media));
\t\t\tbreak;
\t\t}
\t\tcase __GeaStaticCssTapeKind::Color: {
\t\t\tconst auto &op = *colorIt++;
\t\t\t__gea_stylesheet.registerStaticColorRule(__gea_css_tape_selector_kind(op.selectorKind), __gea_css_tape_string(op.selector), static_cast<gea::embedded::ui::StaticStyleColorProperty>(op.property), op.r, op.g, op.b, op.a, __gea_css_tape_string(op.media));
\t\t\tbreak;
\t\t}
\t\tcase __GeaStaticCssTapeKind::ColorVar: {
\t\t\tconst auto &op = *colorVarIt++;
\t\t\t__gea_stylesheet.registerStaticColorVarRule(__gea_css_tape_selector_kind(op.selectorKind), __gea_css_tape_string(op.selector), static_cast<gea::embedded::ui::StaticStyleColorProperty>(op.property), __gea_css_tape_string(op.name), op.hasFallback, op.r, op.g, op.b, op.a, __gea_css_tape_string(op.media));
\t\t\tbreak;
\t\t}
\t\tcase __GeaStaticCssTapeKind::CustomLength: {
\t\t\tconst auto &op = *customLengthIt++;
\t\t\t__gea_stylesheet.registerStaticCustomLengthRule(__gea_css_tape_selector_kind(op.selectorKind), __gea_css_tape_string(op.selector), __gea_css_tape_string(op.name), __gea_css_tape_length(op.unit, op.value), __gea_css_tape_string(op.media));
\t\t\tbreak;
\t\t}
\t\tcase __GeaStaticCssTapeKind::CustomColor: {
\t\t\tconst auto &op = *customColorIt++;
\t\t\t__gea_stylesheet.registerStaticCustomColorRule(__gea_css_tape_selector_kind(op.selectorKind), __gea_css_tape_string(op.selector), __gea_css_tape_string(op.name), op.r, op.g, op.b, op.a, __gea_css_tape_string(op.media));
\t\t\tbreak;
\t\t}
\t\tcase __GeaStaticCssTapeKind::BoxShadowNone: {
\t\t\tconst auto &op = *boxShadowNoneIt++;
\t\t\t__gea_stylesheet.registerStaticBoxShadowNoneRule(__gea_css_tape_selector_kind(op.selectorKind), __gea_css_tape_string(op.selector), __gea_css_tape_string(op.media));
\t\t\tbreak;
\t\t}
\t\tcase __GeaStaticCssTapeKind::FontFamily: {
\t\t\tconst auto &op = *fontFamilyIt++;
\t\t\t__gea_stylesheet.registerStaticFontFamilyRule(__gea_css_tape_selector_kind(op.selectorKind), __gea_css_tape_string(op.selector), __gea_css_tape_string(op.name), __gea_css_tape_string(op.media));
\t\t\tbreak;
\t\t}
\t\tcase __GeaStaticCssTapeKind::FilterBlur: {
\t\t\tconst auto &op = *filterBlurIt++;
\t\t\t__gea_stylesheet.registerStaticFilterBlurRule(__gea_css_tape_selector_kind(op.selectorKind), __gea_css_tape_string(op.selector), __gea_css_tape_length(op.unit, op.value), __gea_css_tape_string(op.media));
\t\t\tbreak;
\t\t}
\t\tcase __GeaStaticCssTapeKind::LineHeight: {
\t\t\tconst auto &op = *lineHeightIt++;
\t\t\t__gea_stylesheet.registerStaticLineHeightRule(__gea_css_tape_selector_kind(op.selectorKind), __gea_css_tape_string(op.selector), static_cast<gea::embedded::ui::StaticStyleLineHeightKind>(op.kind), __gea_css_tape_length(op.unit, op.value), __gea_css_tape_string(op.media));
\t\t\tbreak;
\t\t}
\t\t}
\t}
};
`
}

function compactStaticCssTapeCalls(calls) {
  if (!useStaticCssTape || !useStaticCssRules) return { calls, usedTape: false }
  const kinds = [
    'Property',
    'Length',
    'LengthSpec',
    'Color',
    'ColorVar',
    'CustomLength',
    'CustomColor',
    'BoxShadowNone',
    'FontFamily',
    'FilterBlur',
    'LineHeight',
  ]
  const out = []
  const stringLiterals = ['nullptr']
  const stringIds = new Map([['nullptr', 0]])
  const stringIndex = (value) => {
    const literal = value === 'nullptr' ? 'nullptr' : value
    const cached = stringIds.get(literal)
    if (cached !== undefined) return cached
    const id = stringLiterals.length
    stringIds.set(literal, id)
    stringLiterals.push(literal)
    return id
  }
  let chunk = []
  let usedTape = false
  const flush = () => {
    if (chunk.length === 0) return
    if (chunk.length < staticCssTapeMinChunk) {
      out.push(...chunk.map((entry) => entry.call))
      chunk = []
      return
    }
    usedTape = true
    const order = chunk.map((entry) => `__GeaStaticCssTapeKind::${entry.kind}`).join(', ')
    const opTypes = {
      Property: '__GeaStaticCssPropertyOp',
      Length: '__GeaStaticCssLengthOp',
      LengthSpec: '__GeaStaticCssLengthOp',
      Color: '__GeaStaticCssColorOp',
      ColorVar: '__GeaStaticCssColorVarOp',
      CustomLength: '__GeaStaticCssCustomLengthOp',
      CustomColor: '__GeaStaticCssCustomColorOp',
      BoxShadowNone: '__GeaStaticCssSelectorOnlyOp',
      FontFamily: '__GeaStaticCssNamedOp',
      FilterBlur: '__GeaStaticCssLengthOnlyOp',
      LineHeight: '__GeaStaticCssLineHeightOp',
    }
    const arrays = [`\tstatic constexpr __GeaStaticCssTapeKind order[] = {${order}};`]
    const pointers = kinds.map((kind) => {
      const entries = chunk.filter((entry) => entry.kind === kind).map((entry) => entry.code)
      if (entries.length === 0) return 'nullptr'
      arrays.push(`\tstatic constexpr ${opTypes[kind]} ops${kind}[] = {\n${entries.map((entry) => `\t\t${entry}`).join(',\n')}\n\t};`)
      return `ops${kind}`
    })
    out.push(`{\n${arrays.join('\n')}\n\t__gea_register_static_css_tape({${chunk.length}, order, ${pointers.join(', ')}});\n}`)
    chunk = []
  }
  for (const call of calls) {
    const entry = staticCssTapeEntry(call, stringIndex)
    if (entry) {
      chunk.push(entry)
      continue
    }
    flush()
    out.push(call)
  }
  flush()
  return { calls: out, usedTape, stringLiterals }
}

let staticCssRegistrationCalls = []
function cssRegistrationCode(viteOutDir) {
  const calls = []
  staticCssLengthExpressionCounter = 0
  staticCssLengthExpressionCache = new Map()
  staticCssSelectorPlanCache = new Set()
  staticCssMediaConditionPlanCache = new Set()
  for (const file of findCssFiles(viteOutDir)) {
    const css = stripCssComments(fs.readFileSync(file, 'utf8'))
    const withoutKeyframes = extractKeyframeBlocks(css)
    emitCssKeyframes(withoutKeyframes.keyframes, calls)
    const { base, media } = extractMediaBlocks(withoutKeyframes.base)
    // Base rules first, then @media rules: StyleSheet applies in registration
    // order (last writer wins), so a matching breakpoint overrides the base.
    emitCssRules(stripAtBlocks(base), '', calls)
    for (const block of media) {
      emitCssRules(stripAtBlocks(block.body), block.condition, calls)
    }
  }
  // The calls themselves, before the tape packs them: a target that resolves
  // styles at build time (the Pebble compiled UI) reads these rather than
  // re-deriving the cascade from the CSS text.
  staticCssRegistrationCalls = calls
  if (calls.length === 0) return ''
  const compacted = compactStaticCssTapeCalls(calls)
  const tapePrelude = compacted.usedTape ? staticCssTapePrelude(compacted.stringLiterals) : ''
  return `{\nauto &__gea_stylesheet = gea::embedded::ui::StyleSheet::instance();\n${tapePrelude}__gea_stylesheet.beginRuleRegistrationBatch();\n${compacted.calls.join('\n')}\n__gea_stylesheet.endRuleRegistrationBatch();\n}\n`
}

const appDir = path.resolve(readOption('--app-dir') ?? fail('missing --app-dir <dir>'))
const outDir = path.resolve(readOption('--out-dir') ?? fail('missing --out-dir <dir>'))
acquireOutputPipelineLock(outDir)
// Which geatsc to compile with. `--geatsc-bin` wins; `GEA_GEATSC_BIN` is the
// same choice expressed as an environment variable, for callers that cannot
// add a flag -- a test harness that shells out to several runners, say. It is
// the variable `targets/web/build-web.sh` already forwards as `--geatsc-bin`,
// so this only lets it reach here directly. Unset, everything resolves exactly
// as before: the installed `@geastack/compiler`'s own `dist/cli.js`.
//
// The point is being able to FREEZE the compiler for the length of a
// measurement run. `dist/` is shared and can be rebuilt by anyone at any
// moment; a run whose rows were compiled by different compilers is not one
// compass, and mtimes alone cannot tell a real change from a no-op rebuild.
const geatscBin = path.resolve(readOption('--geatsc-bin') ?? process.env.GEA_GEATSC_BIN ?? path.join(path.dirname(requireFromLib.resolve('@geastack/compiler/package.json')), 'dist/cli.js'))
const viteBin = path.resolve(readOption('--vite-bin') ?? path.join(path.dirname(requireFromLib.resolve('vite/package.json')), JSON.parse(fs.readFileSync(requireFromLib.resolve('vite/package.json'),'utf8')).bin.vite))
const viteOutDir = path.resolve(readOption('--vite-out-dir') ?? path.join(outDir, 'dist'))
const bundlePath = path.resolve(readOption('--bundle-out') ?? path.join(outDir, 'gea-vite-bundle.js'))
const geaIrPath = path.resolve(readOption('--gea-ir-out') ?? path.join(viteOutDir, 'gea-ir.json'))
const moduleGraphOutOption = readOption('--module-graph-out') ?? process.env.GEA_VITE_MODULE_GRAPH_OUT
const moduleGraphOnly = hasFlag('--module-graph-only')
const moduleGraphCompileRequested = hasFlag('--compile-module-graph')
const moduleGraphCompileDisabled = hasFlag('--no-compile-module-graph')
const geaPluginSpecifier = path.resolve(readOption('--geatsc-gea-plugin') ?? requireFromLib.resolve('@geastack/geatsc-plugin-gea'))
const extraGeatscPlugins = readAllOptions('--extra-geatsc-plugin').map((plugin) => path.resolve(plugin))
const appendJsFiles = readAllOptions('--append-js').map((file) => path.resolve(file))
const entry = readOption('--entry') ?? 'index.tsx'
const entrySymbol = readOption('--entry-symbol')
const cppBoard = readOption('--cpp-board')
const cxxStandard = readOption('--cxx-standard')
const pixelPanelEndian = readOption('--pixel-panel-endian')
const cppPreludeSymbol = readOption('--cpp-prelude-symbol')
const fontSymbolPrefix = readOption('--font-symbol-prefix')
const fontViewportWidths = readAllOptions('--font-viewport-width')
const fontViewportHeights = readAllOptions('--font-viewport-height')
const fontDevicePixelRatios = readAllOptions('--font-device-pixel-ratio')
const runtimeTtfFonts = hasFlag('--runtime-ttf-fonts')

// Physical panel resolution per board, mirroring each target's
// GEA_EMBEDDED_DISPLAY_WIDTH/HEIGHT (the lib/gea-embedded/include/display.h
// default is 410x502; boards override via target_compile_definitions). The
// font generator pre-rasterizes viewport-relative font sizes (vw/vh/vmin/vmax)
// and needs the real viewport to bake glyphs at the exact pixel size the
// runtime's parseLengthForNode will request. Resolved from --cpp-board so the
// board CMakeLists need not thread dimensions through; an explicit
// --font-viewport-{width,height} still wins for non-board callers.
const BOARD_VIEWPORTS = {
  'esp32-s3-epaper-1.54': { width: 200, height: 200 },
  'esp32-m5stack-m5paper': { width: 540, height: 960 },
  'esp32-s3-lilygo-t5-epaper-4.7': { width: 540, height: 960 },
  'esp32-s3-m5stack-papers3': { width: 540, height: 960 },
  'esp32-s3-m5stack-sticks3': { width: 135, height: 240 },
  'esp32-s3-elecrow-rotary-2.1': { width: 480, height: 480 },
  'esp32-s3-touch-amoled-2.06': { width: 410, height: 502 },
  'esp32-s3-touch-amoled-1.8': { width: 368, height: 448 },
  'esp32-s3-lilygo-t-display-s3-long': { width: 180, height: 640 },
  'esp32-p4-waveshare-touch-lcd-7': { width: 720, height: 1280 },
  'esp32-p4-waveshare-touch-lcd-3.5': { width: 320, height: 480 },
  'waveshare-rp2350-amoled-2.41': { width: 450, height: 600 },
}
const boardViewport = cppBoard ? BOARD_VIEWPORTS[cppBoard] : undefined
const effectiveFontViewportWidths = fontViewportWidths.length > 0
  ? fontViewportWidths
  : boardViewport
    ? [String(boardViewport.width)]
    : []
const effectiveFontViewportHeights = fontViewportHeights.length > 0
  ? fontViewportHeights
  : boardViewport
    ? [String(boardViewport.height)]
    : []
// Glyph antialiasing per board. 1-bit B/W e-paper reads best with crisp,
// non-antialiased glyphs — sample at 1:1 (supersample 1) so every glyph pixel
// is fully on/off instead of a thresholded AA edge. Grayscale/color panels keep
// the default 4x supersampled AA coverage. An explicit --font-supersample wins.
const BOARD_FONT_SUPERSAMPLE = {
  'esp32-s3-epaper-1.54': 1,
}
const fontSupersampleOverride = readOption('--font-supersample')
const effectiveFontSupersample = fontSupersampleOverride
  ?? (cppBoard && BOARD_FONT_SUPERSAMPLE[cppBoard] !== undefined ? String(BOARD_FONT_SUPERSAMPLE[cppBoard]) : undefined)
// Baked glyph atlas depth per board. 8 bits of coverage per pixel is the
// default. A panel that can only show four gray levels gains nothing from the
// other six bits, and reading the atlas out of XIP flash costs four times the
// bandwidth — bake those boards at 2 bits/px instead, quantized offline. An
// explicit --font-atlas-bits wins.
const BOARD_FONT_ATLAS_BITS = {
  'esp32-c3-xteink-x3': 2,
  // PaperS3 has a 16-level framebuffer, but glyph antialiasing only needs the
  // four offline-quantized coverage levels. This makes the existing nonlinear
  // edge treatment (48/128/208 thresholds plus stem/speckle fixups) part of the
  // baked atlas and avoids carrying 8-bit coverage through XIP at runtime.
  'esp32-s3-m5stack-papers3': 2,
}
const fontAtlasBitsOverride = readOption('--font-atlas-bits')
const effectiveFontAtlasBits = fontAtlasBitsOverride
  ?? (cppBoard && BOARD_FONT_ATLAS_BITS[cppBoard] !== undefined ? String(BOARD_FONT_ATLAS_BITS[cppBoard]) : undefined)
const geaEmbeddedCompat = hasFlag('--gea-embedded-compat') || usesGeaEmbeddedCompat(appDir)
const appleNative = !hasFlag('--no-apple-native') && (hasFlag('--apple-native') || usesAppleNative(appDir))
const moduleGraphCompile = !moduleGraphCompileDisabled && (moduleGraphCompileRequested || geaEmbeddedCompat || appleNative)
const moduleGraphEnabled = moduleGraphOnly || moduleGraphCompile || !!moduleGraphOutOption
const moduleGraphOutDir = moduleGraphOutOption
  ? path.resolve(moduleGraphOutOption)
  : moduleGraphEnabled
    ? path.join(outDir, 'module-graph')
    : ''
let viteConfig = geaEmbeddedCompat
  ? path.join(outDir, 'gea-embedded-compat.vite.config.mjs')
  : path.resolve(readOption('--vite-config') ?? path.join(appDir, 'vite.config.ts'))
const appleNativePluginSpecifier = path.resolve(readOption('--geatsc-apple-native-plugin') ?? tryResolve('@geastack/geatsc-plugin-apple-native'))

if (!geaEmbeddedCompat && !fs.existsSync(viteConfig)) fail(`missing Vite config: ${viteConfig}`)
if (!fs.existsSync(viteBin)) fail(`missing Vite CLI: ${viteBin}`)
if (!fs.existsSync(geatscBin)) fail(`missing geatsc CLI: ${geatscBin}`)
if (appleNative && !fs.existsSync(appleNativePluginSpecifier)) fail(`missing Apple native geatsc plugin: ${appleNativePluginSpecifier}`)
for (const plugin of extraGeatscPlugins) {
  if (!fs.existsSync(plugin)) fail(`missing extra geatsc plugin: ${plugin}`)
}

fs.mkdirSync(outDir, { recursive: true })
writeDisplayConfigHeader(outDir, collectDisplayConfig(appDir, entry))

// Where the entry actually lives once compat staging has copied the sources.
// The module graph is rooted at the staged tree, so geatsc must be pointed at
// the staged entry too: an --entry naming the original app dir matches no
// module in the graph, which silently empties the reachability root set.
let stagedEntryFile = null

// Where the font generator looks for @font-face. It scans a DIRECTORY, so the
// app dir only works while every stylesheet lives under it. An app whose entry
// imports components from a sibling tree declares its face there, and the app
// dir holds no CSS at all -- the atlas then comes out empty and every string
// falls back to the built-in bitmap font. Compat staging has already gathered
// the real source set (the app dir plus each escaped import dir, mirrored at
// their original depth so relative url()s still resolve), so point the scan at
// that instead. Identical for an app that is its own staging root.
let fontSourceDir = appDir

if (geaEmbeddedCompat) {
  const compatSrcDir = path.join(outDir, 'gea-embedded-compat-src')
  fs.rmSync(compatSrcDir, { recursive: true, force: true })
  // Relative imports may escape the app dir (examples/shared components).
  // Mirror the escaped trees at their original depth below the common
  // ancestor so the staged sources keep resolving each other, and aim the
  // vite entry at the app's staged location.
  const escapedDirs = escapedCompatImportDirs(appDir, entry)
  // App .env vars, baked into the staged sources as process.env.<KEY> literals.
  const envDefines = dotEnvDefines(appDir)
  let compatEntry = entry
  let stagingRoot = appDir
  if (escapedDirs.length === 0) {
    copyCompatSourceTree(appDir, compatSrcDir, envDefines)
  } else {
    stagingRoot = commonAncestorDir([appDir, ...escapedDirs])
    copyCompatSourceTree(appDir, path.join(compatSrcDir, path.relative(stagingRoot, appDir)), envDefines)
    for (const dir of escapedDirs) copyCompatSourceTree(dir, path.join(compatSrcDir, path.relative(stagingRoot, dir)), envDefines)
    compatEntry = path.join(path.relative(stagingRoot, appDir), entry)
  }
  linkCompatNodeModules(appDir, path.join(compatSrcDir, path.dirname(compatEntry)), compatSrcDir)
  stateRuntimeAliasToTypeScript(path.join(compatSrcDir, path.relative(stagingRoot, appDir)), compatSrcDir, stagingRoot)
  if (moduleGraphCompile) {
    compatEntry = writeAppendJsCompatEntry({ compatSrcDir, compatEntry, appendFiles: appendJsFiles })
  }
  stagedEntryFile = path.join(compatSrcDir, compatEntry)
  fontSourceDir = compatSrcDir
  writeCompatViteConfig({ configPath: viteConfig, compatSrcDir, appDir, entry: compatEntry, viteOutDir, geaIrPath, appleNative, moduleGraphOutDir, envDefines })
} else if (appleNative) {
  const baseViteConfig = viteConfig
  viteConfig = path.join(outDir, 'gea-apple-native.vite.config.mjs')
  writeAppleNativeViteConfig({ configPath: viteConfig, baseConfigPath: baseViteConfig, appDir, moduleGraphOutDir })
}

run('node', [
  viteBin,
  'build',
  '--config',
  viteConfig,
  '--outDir',
  viteOutDir,
  '--emptyOutDir',
  '--minify',
  'false',
], {
  env: {
    ...process.env,
    GEA_IR_OUT: geaIrPath,
    ...(moduleGraphOutDir ? { GEA_VITE_MODULE_GRAPH_OUT: moduleGraphOutDir } : {}),
  },
})
if (moduleGraphOnly) {
  if (!moduleGraphOutDir) fail('--module-graph-only requires --module-graph-out <dir> or GEA_VITE_MODULE_GRAPH_OUT')
  process.stdout.write(`module-graph=${path.join(moduleGraphOutDir, 'gea-module-graph.json')}\n`)
  process.stdout.write(`module-graph-sources=${path.join(moduleGraphOutDir, 'sources')}\n`)
  process.exit(0)
}

const viteBundle = findFirstJsFile(viteOutDir)
if (!viteBundle) fail(`Vite produced no JS bundle in ${viteOutDir}`)
const cssPrelude = geaEmbeddedCompat ? cssRegistrationCode(viteOutDir) : ''
const cssPreludePath = path.join(outDir, 'gea-style-registration.cppfrag')
fs.writeFileSync(path.join(outDir, 'gea-style-registration.calls.json'), JSON.stringify(staticCssRegistrationCalls, null, 1) + '\n')
if (cssPrelude || cppPreludeSymbol) {
  fs.writeFileSync(cssPreludePath, cssPrelude)
} else {
  fs.rmSync(cssPreludePath, { force: true })
}
run('node', [
  path.join(coreRoot, 'scripts/generate-gea-embedded-fonts.mjs'),
  '--app-dir',
  fontSourceDir,
  '--css-dir',
  viteOutDir,
  '--out-cpp',
  path.join(outDir, 'gea_embedded_font_generated.cpp'),
  '--out-h',
  path.join(outDir, 'gea_embedded_font_generated.h'),
  ...effectiveFontViewportWidths.flatMap((width) => ['--viewport-width', width]),
  ...effectiveFontViewportHeights.flatMap((height) => ['--viewport-height', height]),
  ...fontDevicePixelRatios.flatMap((ratio) => ['--device-pixel-ratio', ratio]),
  ...(effectiveFontSupersample ? ['--supersample', effectiveFontSupersample] : []),
  ...(effectiveFontAtlasBits ? ['--atlas-bits', effectiveFontAtlasBits] : []),
  ...(fontSymbolPrefix ? ['--symbol-prefix', fontSymbolPrefix] : []),
  ...(runtimeTtfFonts ? ['--runtime-ttf-fonts'] : []),
])
// Embed app assets (images referenced as `<img src="...">`) as `.rodata` linked
// into the app binary, registered by path. Like the font TU above, this always
// emits a (possibly empty) gea_embedded_assets_generated.cpp so every target's
// source list can include it unconditionally. Symbols live in an anonymous
// namespace, so several isolated programs' asset TUs can be linked together (a
// launcher bundle) without colliding.
run('node', [
  path.join(coreRoot, 'scripts/generate-gea-embedded-assets.mjs'),
  '--app-dir',
  appDir,
  '--out-cpp',
  path.join(outDir, 'gea_embedded_assets_generated.cpp'),
])
const typeHints = collectBundledTypeHints(appDir, entry)
const typeHintsPath = path.join(path.dirname(bundlePath), 'gea-type-hints.d.ts')
let bundledCode = fs.readFileSync(viteBundle, 'utf8')
let typeHintsReferencePath = ''
const typeHintParts = [...(typeHints.imports ?? []), ...typeHints.declarations]
if (typeHintParts.length > 0) {
  fs.writeFileSync(typeHintsPath, `${typeHintParts.join('\n\n')}\n`)
  typeHintsReferencePath = `./${path.basename(typeHintsPath)}`
} else {
  fs.rmSync(typeHintsPath, { force: true })
}
bundledCode = applyBundledTypeHints(bundledCode, typeHints, typeHintsReferencePath)
// The vite-plugin's lean ReactiveComponent transform emits side-effectful
// `(globalThis.__GEA_IR_KEEP__ ||= []).push(Child, …)` keep-alives so the
// bundler can't tree-shake IR-live child classes and the store globals they
// reference. The references did their job during bundling — strip the marker
// statements so geatsc never compiles a globalThis property write.
bundledCode = bundledCode.replace(/^.*__GEA_IR_KEEP__.*$\n?/gm, '')
// Compiled-template root caches (`var _tplN_root = null`) hold a cached node
// that later gets `.cloneNode(true)` — typing them as Node makes geatsc store
// them as a native NodeHandle so cloneNode/firstChild/childNodes dispatch to
// the typed tree methods instead of falling onto an opaque boxed object (the
// untyped null inferred gea_cpp_value, whose generic boxing has no DOM method
// table — the app mounted nothing and the screen stayed black).
bundledCode = bundledCode.replace(/\bvar (_tpl\d+_root) = null;/g, '/** @type {Node} */ var $1 = null;')
// JS `(cache || (cache = make()))` returns the VALUE; on native NodeHandles
// the C++ `||` lowering yields bool — statement-ize the idiom instead.
bundledCode = bundledCode.replace(/\((_tpl\d+_root) \|\| \(\1 = (_tpl\d+_create)\(\)\)\)\.cloneNode\(true\)/g,
  '($1 = $1 || $2(), $1).cloneNode(true)')
fs.writeFileSync(bundlePath, bundledCode)
if (!moduleGraphCompile) {
  for (const appendFile of appendJsFiles) {
    if (!fs.existsSync(appendFile)) fail(`missing appended JS file: ${appendFile}`)
    fs.appendFileSync(bundlePath, `\n${fs.readFileSync(appendFile, 'utf8')}\n`)
  }
}
// Module-graph snapshots are captured pre-type-strip, so they keep their
// source annotations — EXCEPT the `el = null` field the lean ReactiveComponent
// transform injects untyped after stripping the `Component<T>` heritage.
// Re-attach the heritage generic as a real TS annotation (`el: T | null`) so
// geatsc lowers `this.el` to the typed native handle instead of a boxed value.
if (moduleGraphCompile && moduleGraphOutDir) {
  restoreModuleGraphElAnnotations(moduleGraphOutDir, typeHints.propertyHints)
}

const applePlatform = readOption('--apple-platform') ?? ''
const appleNativeMetadataPath = appleNative ? await writeAppleNativeMetadata(outDir, appDir, applePlatform) : ''
const geatscArgs = [
  geatscBin,
  moduleGraphCompile ? 'compile-module-graph' : 'compile',
  moduleGraphCompile ? path.join(moduleGraphOutDir, 'gea-module-graph.json') : bundlePath,
  '--out-dir',
  outDir,
  '--plugin',
  geaPluginSpecifier,
  '--plugin-option',
  `gea.ir=${geaIrPath}`,
  '--plugin-option',
  `gea.microtasks-namespace=${microtasksNamespaceForEntry(entrySymbol)}`,
]
if (!moduleGraphCompile) geatscArgs.push('--target', 'cpp')
if (moduleGraphCompile) {
  geatscArgs.push('--entry', stagedEntryFile ?? path.join(appDir, entry), '--module-graph-stage', 'hybrid')
}
if (appleNative) {
  geatscArgs.push('--plugin', appleNativePluginSpecifier, '--plugin-option', `apple.metadata=${appleNativeMetadataPath}`)
}
for (const plugin of extraGeatscPlugins) {
  geatscArgs.push('--plugin', plugin)
}
if (cssPrelude || cppPreludeSymbol) geatscArgs.push('--plugin-option', `gea.cpp-prelude=${cssPreludePath}`)
if (cppPreludeSymbol) geatscArgs.push('--plugin-option', `gea.cpp-prelude-symbol=${cppPreludeSymbol}`)
if (pixelPanelEndian) geatscArgs.push('--plugin-option', `gea.pixel-panel-endian=${pixelPanelEndian}`)
if (cppBoard) geatscArgs.push('--cpp-board', cppBoard)
if (cxxStandard) geatscArgs.push('--cxx-standard', cxxStandard)
if (hasFlag('--allow-any')) geatscArgs.push('--allow-any')
if (entrySymbol) geatscArgs.push('--entry-symbol', entrySymbol)
if (hasFlag('--isolate-symbols')) geatscArgs.push('--isolate-symbols')
// One C++ unit per source module plus a shared header, instead of one unit for
// the whole program. Every target already reads `geatsc-sources.txt` as a LIST,
// so nothing downstream changes; see the compiler's docs/TRANSLATION-UNITS.md
// for what it buys (parallel and incremental C++ builds) and what it costs.
// The env var is honored beside the flag so a target need not plumb one: every
// build in the tree goes through this script, and the boards' CMakeLists spell
// their geatsc command line by hand, so an env var reaches all of them at once
// (`GEA_PER_FILE_UNITS=1 <any build>`). build-macos.sh sets the flag itself and
// folds the variable into its freshness signature, because a layout change
// invalidates every generated file.
if (process.env.GEA_CPP_TRANSLATION_UNITS === 'balanced') geatscArgs.push('--translation-units', 'balanced')
else if (hasFlag('--per-file-units') || process.env.GEA_PER_FILE_UNITS === '1') geatscArgs.push('--translation-units', 'per-file')
if (hasFlag('--gea-ir-backend') || hasFlag('--gea-replace-renderers')) {
  geatscArgs.push('--plugin-option', 'gea.replace-renderers=true')
}
// The whole-program analysis of a three.js app (a 284-module graph)
// peaks well above node's default heap cap: on 2026-09-14 this exact spawn
// died with `Ineffective mark-compacts near heap limit` after five minutes,
// and the build reported "C++ generation failed". The compiler's own
// measurement scripts are always run with this
// limit; the build has to state the same one, not depend on the caller's
// NODE_OPTIONS.
run('node', ['--max-old-space-size=32768', ...geatscArgs])

process.stdout.write(`gea-vite-bundle=${bundlePath}\n`)
if (moduleGraphOutDir) process.stdout.write(`module-graph=${path.join(moduleGraphOutDir, 'gea-module-graph.json')}\n`)
const geatscSourceListPath = path.join(outDir, 'geatsc-sources.txt')
process.stdout.write(`geatsc-sources=${geatscSourceListPath}\n`)
if (fs.existsSync(geatscSourceListPath)) {
  const [entrySource] = fs.readFileSync(geatscSourceListPath, 'utf8').split(/\r?\n/).filter(Boolean)
  if (entrySource) process.stdout.write(`geatsc-entry-source=${entrySource}\n`)
}

function microtasksNamespaceForEntry(symbol) {
  if (symbol && symbol.endsWith('_top_level')) {
    return `gea::framework::app::generated::${symbol.replace(/_top_level$/, '')}`
  }
  return 'gea::framework::app::generated'
}
