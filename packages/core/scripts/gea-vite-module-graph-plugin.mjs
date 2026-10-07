import { describeStaticAsset } from './gea-static-asset.mjs'
import crypto from 'node:crypto'
import fs from 'node:fs'
import path from 'node:path'

const JS_LIKE_RE = /\.(?:mjs|cjs|js|jsx|ts|tsx)$/i
const STATIC_ASSET_RE = /\.(?:apng|bmp|png|jpe?g|jfif|pjpeg|pjp|gif|svg|ico|webp|avif|cur|jxl|mp4|webm|ogg|mp3|wav|flac|aac|opus|mov|m4a|vtt|woff2?|eot|ttf|otf|webmanifest|pdf|txt|glb|gltf|hdr)$/i
const STATIC_ASSET_MODULE_SUFFIX = '.geaassetmodule.js'
const RAW_MODULE_SUFFIX = '.gearawmodule.js'
const STATIC_IMPORT_RE = /\bimport\s+(?:[^'"()]*?\s+from\s*)?["']([^"']+)["']/g
const EXPORT_FROM_RE = /\bexport\s+(?:[^'"]*?\s+from\s*)["']([^"']+)["']/g
const DYNAMIC_IMPORT_RE = /\bimport\s*\(\s*["']([^"']+)["']\s*\)/g

// Native targets replace browser-only package surfaces with reusable native
// packages. Keeping these aliases here makes the target choice part of Core's
// generated native build rather than an application-specific Vite override.
export function geaAppleNativeModuleAliases(options = {}) {
  const aliases = []
  if (options.threeWebGLAnimationModule) {
    if (!path.isAbsolute(options.threeWebGLAnimationModule)) {
      throw new TypeError('threeWebGLAnimationModule must be an absolute module resolved from the application package boundary')
    }
    aliases.push({
      find: /^\.\/webgl\/WebGLAnimation\.js$/,
      replacement: options.threeWebGLAnimationModule,
    })
  }
  if (options.threeWebXRManagerModule) {
    if (!path.isAbsolute(options.threeWebXRManagerModule)) {
      throw new TypeError('threeWebXRManagerModule must be an absolute module resolved from the application package boundary')
    }
    aliases.push({
      find: /^\.\/webxr\/WebXRManager\.js$/,
      replacement: options.threeWebXRManagerModule,
    })
  }
  if (options.threeUtilsModule) {
    if (!path.isAbsolute(options.threeUtilsModule)) {
      throw new TypeError('threeUtilsModule must be an absolute module resolved from the application package boundary')
    }
    aliases.push({
      find: /^(?:\.\.\/)+utils\.js$/,
      replacement: options.threeUtilsModule,
    })
  }
  if (options.troikaThreeTextModule) {
    // Vite aliases are not a guaranteed recursive resolver chain. Freeze the
    // native module selected at the application boundary into one absolute hop.
    if (!path.isAbsolute(options.troikaThreeTextModule)) {
      throw new TypeError('troikaThreeTextModule must be an absolute module resolved from the application package boundary')
    }
    aliases.push({
      find: /^troika-three-text$/,
      replacement: options.troikaThreeTextModule,
    })
  }
  if (options.threeSrcDir) {
    if (!path.isAbsolute(options.threeSrcDir)) {
      throw new TypeError('threeSrcDir must be an absolute directory resolved from the application package boundary')
    }
    aliases.push({
      find: /^three\/src\/(.+)$/,
      replacement: path.join(options.threeSrcDir, '$1'),
    })
  }
  return aliases
}

// An app that opts into @geastack/gea-threejs (three.js ported to strictly
// typed TypeScript, with a native software renderer) by setting
// `"gea": { "geaThreejs": true }` gets three's import surface redirected to
// the port on non-apple native targets: bare `three`,
// every `three/src/*.js` path (to its `.ts` source), and `troika-three-text`
// (to the native atlas text, whose own `three/src` imports land on the port
// through the same aliases).
export function geaThreeModuleAliases(options = {}) {
  const aliases = []
  if (!options.geaThreeSrcDir) return aliases
  if (!path.isAbsolute(options.geaThreeSrcDir)) {
    throw new TypeError('geaThreeSrcDir must be an absolute directory resolved from the application package boundary')
  }
  aliases.push({ find: /^three$/, replacement: path.join(options.geaThreeSrcDir, 'Three.ts') })
  aliases.push({ find: /^three\/src\/(.+)\.js$/, replacement: path.join(options.geaThreeSrcDir, '$1.ts') })
  if (options.troikaThreeTextModule) {
    if (!path.isAbsolute(options.troikaThreeTextModule)) {
      throw new TypeError('troikaThreeTextModule must be an absolute module resolved from the application package boundary')
    }
    aliases.push({ find: /^troika-three-text$/, replacement: options.troikaThreeTextModule })
  }
  return aliases
}

export function geaModuleGraphPlugins(options = {}) {
  const outDir = options.outDir || process.env.GEA_VITE_MODULE_GRAPH_OUT || ''
  if (!outDir) return []

  const state = {
    outDir: path.resolve(outDir),
    root: '',
    original: new Map(),
    transformed: new Map(),
    imports: new Map(),
    assets: new Map(),
    rawModules: new Map(),
    // GEA_MODULE_GRAPH_ENTRY_REACHABLE_ONLY=0 keeps the unpruned graph, so a
    // build can be A/B'd against the reachability filter without editing the
    // generated vite config.
    entryReachableOnly: options.entryReachableOnly === true &&
      process.env.GEA_MODULE_GRAPH_ENTRY_REACHABLE_ONLY !== '0',
  }

  return [
    {
      name: 'gea-module-graph-static-assets',
      enforce: 'pre',
      apply: 'build',
      configResolved(config) {
        state.root = config.root
      },
      async resolveId(source, importer) {
        // Vite normally turns a plain static-asset import into a JS module that
        // exports the emitted browser URL. The native compiler consumes source
        // snapshots instead of the browser bundle, so expose the same binding
        // through a compiler-visible synthetic JS module. Raw imports export
        // file contents instead; other query imports retain Vite semantics.
        const normalizedImporter = normalizeModuleId(importer)
        const raw = source.endsWith('?raw')
        const assetSpecifier = raw ? source.slice(0, -4) : source
        if (
          !importer ||
          !shouldTrackModule(normalizedImporter) ||
          assetSpecifier.includes('?') ||
          assetSpecifier.includes('#') ||
          (!raw && !STATIC_ASSET_RE.test(assetSpecifier))
        ) return null

        let resolvedId = ''
        const resolved = await this.resolve(assetSpecifier, importer, { skipSelf: true })
        if (resolved && !resolved.external) resolvedId = normalizeModuleId(resolved.id)
        if (!resolvedId && assetSpecifier.startsWith('.')) {
          resolvedId = path.resolve(path.dirname(normalizedImporter), assetSpecifier)
        }
        if (!path.isAbsolute(resolvedId) || !fs.existsSync(resolvedId) || !fs.statSync(resolvedId).isFile()) return null

        const assetSource = fs.realpathSync(resolvedId)
        if (raw) {
          const moduleId = `${assetSource}${RAW_MODULE_SUFFIX}`
          state.rawModules.set(moduleId, fs.readFileSync(assetSource, 'utf8'))
          return moduleId
        }

        const moduleId = `${assetSource}${STATIC_ASSET_MODULE_SUFFIX}`
        if (!state.assets.has(moduleId)) state.assets.set(moduleId, describeStaticAsset(assetSource))
        return moduleId
      },
      load(id) {
        const normalizedId = normalizeModuleId(id)
        if (state.rawModules.has(normalizedId)) {
          return `export default ${JSON.stringify(state.rawModules.get(normalizedId))}\n`
        }
        const asset = state.assets.get(normalizedId)
        if (!asset) return null
        return `export default ${JSON.stringify(asset.url)}\n`
      },
    },
    {
      name: 'gea-module-graph-original',
      enforce: 'pre',
      apply: 'build',
      configResolved(config) {
        state.root = config.root
      },
      transform(code, id) {
        captureSource(state.original, id, code)
        return null
      },
    },
    {
      name: 'gea-module-graph-transformed',
      // 'pre', not 'post': this plugin is registered AFTER the gea plugin, so a
      // pre-enforced transform still sees the code with the gea component/store
      // transforms applied — but BEFORE vite's core TS transform strips every
      // type annotation. geatsc (the consumer of these snapshots) is a
      // TypeScript compiler: erased annotations are pure loss for it. A `.tsx`
      // module compiled from the post-strip snapshot lost e.g. `const
      // cacheImgs: GeaEmbeddedImage[] = []` and boxed the whole array into
      // `std::vector<gea_cpp_value>` (maps' tile cache), instead of the typed
      // `std::vector<gea::host::GeaEmbeddedImage>` the annotation lowers to.
      enforce: 'pre',
      apply: 'build',
      configResolved(config) {
        state.root = config.root
      },
      async transform(code, id) {
        captureSource(state.transformed, id, code)
        await captureResolvedImports(this, state, id, code)
        return null
      },
      generateBundle(_options, bundle) {
        reportTreeShakenModules(this, state, bundle)
        writeModuleGraph(this, state)
      },
    },
  ]
}

export default geaModuleGraphPlugins

function captureSource(target, id, code) {
  const normalized = normalizeModuleId(id)
  if (!shouldTrackModule(normalized)) return
  target.set(normalized, code)
}

function writeModuleGraph(pluginContext, state) {
  fs.rmSync(state.outDir, { recursive: true, force: true })
  fs.mkdirSync(path.join(state.outDir, 'sources'), { recursive: true })

  const rawIdsByNormalizedId = new Map()
  for (const rawId of pluginContext.getModuleIds()) {
    const id = normalizeModuleId(rawId)
    if (shouldTrackModule(id) && !rawIdsByNormalizedId.has(id)) rawIdsByNormalizedId.set(id, rawId)
  }
  // A framework transform can consume an imported asset while leaving the
  // original TS snapshot's binding intact. Rollup then never loads its module;
  // the native graph still needs the resolved literal module for that binding.
  for (const id of [...state.assets.keys(), ...state.rawModules.keys()]) {
    if (!rawIdsByNormalizedId.has(id)) rawIdsByNormalizedId.set(id, id)
  }
  const modules = []
  const allIds = Array.from(rawIdsByNormalizedId.keys()).sort()
  const reachableIds = state.entryReachableOnly
    ? entryReachableModuleIds(pluginContext, state, rawIdsByNormalizedId, allIds)
    : allIds
  const live = liveModuleIds(state, reachableIds)
  const ids = reachableIds.filter((id) => live.keep.has(id))

  for (const id of ids) {
    const info = pluginContext.getModuleInfo(rawIdsByNormalizedId.get(id))
    const prune = (code) => pruneDeadReExports(code, id, state, live)
    const asset = state.assets.get(id)
    const literal = asset?.url ?? state.rawModules.get(id)
    const syntheticSource = literal === undefined ? undefined : `export default ${JSON.stringify(literal)}\n`
    const originalCode = prune(normalizeSnapshotSourceForGeatsc(state.original.get(id) ?? syntheticSource ?? readFileSource(id)))
    const transformedCode = prune(normalizeSnapshotSourceForGeatsc(state.transformed.get(id) ?? syntheticSource))
    const originalPath = writeSnapshot(state, id, 'original', originalCode)
    const transformedPath = writeSnapshot(state, id, 'transformed', transformedCode)
    modules.push({
      id,
      file: path.isAbsolute(id) ? id : null,
      relativeFile: path.isAbsolute(id) && state.root ? path.relative(state.root, id) : null,
      imports: (state.imports.get(id) ?? []).filter(
        (imported) => !imported.tracked || !imported.resolvedId || live.keep.has(imported.resolvedId),
      ),
      importedIds: normalizeIdList(info?.importedIds ?? []).filter((dependency) => live.keep.has(dependency)),
      dynamicallyImportedIds: normalizeIdList(info?.dynamicallyImportedIds ?? []).filter((dependency) => live.keep.has(dependency)),
      importers: normalizeIdList(info?.importers ?? []).filter((importer) => live.keep.has(importer)),
      isEntry: Boolean(info?.isEntry),
      hasModuleSideEffects: info?.moduleSideEffects ?? null,
      originalSource: originalPath,
      transformedSource: transformedPath,
      originalBytes: originalCode === undefined ? 0 : Buffer.byteLength(originalCode),
      transformedBytes: transformedCode === undefined ? 0 : Buffer.byteLength(transformedCode),
      ...(asset ? {
        assetSource: asset.source,
        assetBytes: asset.bytes,
        assetSha256: asset.sha256,
        assetUrl: asset.url,
      } : {}),
    })
  }

  const assets = modules
    .filter((module) => module.assetSource)
    .map((module) => ({
      moduleId: module.id,
      source: module.assetSource,
      relativeFile: state.root && path.isAbsolute(module.assetSource)
        ? path.relative(state.root, module.assetSource)
        : null,
      bytes: module.assetBytes,
      sha256: module.assetSha256,
      url: module.assetUrl,
    }))

  const manifest = {
    version: 1,
    generatedAt: new Date().toISOString(),
    root: state.root,
    moduleCount: modules.length,
    assetCount: assets.length,
    assets,
    modules,
  }
  fs.writeFileSync(path.join(state.outDir, 'gea-module-graph.json'), `${JSON.stringify(manifest, null, 2)}\n`)
}

/**
 * Report what Rollup's own tree-shaking decided, next to what the graph is
 * about to hand geatsc.
 *
 * The graph is assembled from MODULE-level reachability, and every gea runtime
 * module is reachable because `compiler-runtime.ts` is a pure re-export barrel:
 * one import of the virtual runtime pulls in `keyed-list`, `reactive-html`,
 * `relational-class`, `conditional-truthy` and the rest whether or not the
 * application calls any of them. geatsc then has to lower all of it, so a
 * lowering gap in a module the product never runs blocks the whole build —
 * which is exactly where a real app stopped (a nested function in
 * `conditional-truthy.ts`, a module its App.tsx never imports a name from).
 *
 * Rollup already answered the SYMBOL-level question by the time this hook runs:
 * a module that survived shaking appears in some chunk's `modules` map, and one
 * that did not appear at all is dead code by the bundler's own account. This
 * logs both sets unconditionally — no environment variable — so the size of the
 * gap between "in the graph" and "in the bundle" is visible on every build
 * instead of having to be re-measured by hand.
 */
function reportTreeShakenModules(pluginContext, state, bundle) {
  const rendered = (state.rendered = new Set())
  for (const output of Object.values(bundle ?? {})) {
    if (output?.type !== 'chunk') continue
    for (const id of Object.keys(output.modules ?? {})) {
      const normalized = normalizeModuleId(id)
      if (shouldTrackModule(normalized)) rendered.add(normalized)
    }
  }
  const tracked = []
  for (const rawId of pluginContext.getModuleIds()) {
    const id = normalizeModuleId(rawId)
    if (shouldTrackModule(id) && !tracked.includes(id)) tracked.push(id)
  }
  const shaken = tracked.filter((id) => !rendered.has(id)).sort()
  process.stderr.write(
    `[gea-module-graph] ${tracked.length} tracked module(s), ${rendered.size} rendered by rollup, ${shaken.length} shaken out\n`,
  )
  for (const id of shaken) {
    process.stderr.write(`[gea-module-graph]   shaken-out: ${state.root ? path.relative(state.root, id) : id}\n`)
  }
}

/**
 * The modules the graph actually hands geatsc, and which of them are pure
 * re-export hubs whose dead re-exports must be pruned with them.
 *
 * Rollup's rendered set is the authority on what is live; this function only
 * adds back what would otherwise become UNRESOLVABLE. Two kinds of module are
 * added back:
 *
 *   - a re-export hub (`compiler-runtime.ts`, the `virtual:gea-*` runtime
 *     module) that a surviving module imports through. Rollup inlines the
 *     bindings and never renders the hub itself, but geatsc resolves imports by
 *     module, so the hub has to stay — with its dead re-export lines removed,
 *     which is what `pruneDeadReExports` does.
 *   - any other module a surviving module still imports. Rollup dropping it
 *     while a surviving snapshot names it means the snapshot, not the bundle,
 *     is the binding contract here; keeping it is the fail-safe direction.
 *
 * A hub contributes only rendered modules and further hubs, which is the whole
 * point: `compiler-runtime.ts` re-exports every gea runtime module, so
 * following its edges unconditionally would resurrect the entire runtime and
 * leave the graph exactly as unpruned as before.
 */
function liveModuleIds(state, ids) {
  const tracked = new Set(ids)
  const rendered = state.rendered
  // No bundle information at all (a pipeline that never reached
  // `generateBundle`, or a build that rendered nothing) is safer left intact
  // than silently reduced.
  if (!rendered || rendered.size === 0) return { keep: tracked, hubs: new Set() }
  const keep = new Set([...tracked].filter((id) => rendered.has(id)))
  if (keep.size === 0) return { keep: tracked, hubs: new Set() }

  const hubs = new Set()
  const isHub = (id) => {
    if (hubs.has(id)) return true
    if (!reExportHubIds(state).has(id)) return false
    hubs.add(id)
    return true
  }
  const resurrected = []
  let changed = true
  while (changed) {
    changed = false
    for (const id of [...keep]) {
      const viaHub = isHub(id)
      for (const imported of state.imports.get(id) ?? []) {
        const target = imported.resolvedId
        if (!imported.tracked || !target || !tracked.has(target) || keep.has(target)) continue
        if (viaHub && !rendered.has(target) && !isHub(target)) continue
        keep.add(target)
        resurrected.push(target)
        changed = true
      }
    }
  }

  process.stderr.write(
    `[gea-module-graph] pruned to ${keep.size}/${tracked.size} module(s): ` +
      `${[...keep].filter((id) => rendered.has(id)).length} rendered, ${hubs.size} re-export hub(s), ` +
      `${resurrected.filter((id) => !hubs.has(id)).length} kept because a surviving module still imports them\n`,
  )
  for (const id of resurrected.filter((target) => !hubs.has(target)).sort()) {
    process.stderr.write(`[gea-module-graph]   kept-unrendered: ${state.root ? path.relative(state.root, id) : id}\n`)
  }
  return { keep, hubs }
}

/**
 * Modules whose entire body is imports and `export ... from` — no declarations,
 * no statements. Only these are safe to rewrite by dropping re-export lines.
 */
function reExportHubIds(state) {
  if (state.hubIds) return state.hubIds
  const hubIds = (state.hubIds = new Set())
  for (const [id, code] of state.transformed.size > 0 ? state.transformed : state.original) {
    const source = code ?? ''
    const withoutComments = source.replace(/\/\*[\s\S]*?\*\//g, '').replace(/^[^\S\n]*\/\/.*$/gm, '')
    const remainder = withoutComments
      .replace(/\bimport\s+(?:[^'"()]*?\s+from\s*)?["'][^"']+["']\s*;?/g, '')
      .replace(/\bexport\s+(?:[^'"]*?\s+from\s*)["'][^"']+["']\s*;?/g, '')
    if (remainder.trim().length === 0 && /\bexport\b/.test(withoutComments)) hubIds.add(id)
  }
  return hubIds
}

/**
 * Remove a hub's `export ... from` statements whose target did not survive.
 *
 * Left in place they name modules the graph no longer carries, which is an
 * unresolvable import for geatsc rather than the dead code rollup treated them
 * as. Non-hub modules are returned untouched — this rewrite is only sound for a
 * body that is nothing but re-exports.
 */
function pruneDeadReExports(code, id, state, live) {
  if (code === undefined || !live.hubs.has(id)) return code
  const targetBySpecifier = new Map()
  for (const imported of state.imports.get(id) ?? []) {
    if (imported.resolvedId) targetBySpecifier.set(imported.specifier, imported.resolvedId)
  }
  return code.replace(/\bexport\s+(?:[^'"]*?\s+from\s*)["']([^"']+)["']\s*;?/g, (statement, specifier) => {
    const target = targetBySpecifier.get(specifier)
    return target && !live.keep.has(target) ? '' : statement
  })
}

function entryReachableModuleIds(pluginContext, state, rawIdsByNormalizedId, allIds) {
  const tracked = new Set(allIds)
  const reachable = new Set()
  const pending = []

  for (const id of allIds) {
    const info = pluginContext.getModuleInfo(rawIdsByNormalizedId.get(id))
    if (info?.isEntry) pending.push(id)
  }

  // A malformed or synthetic-only build with no visible entry is safer left
  // intact than silently reduced to an empty compiler graph.
  if (pending.length === 0) return allIds

  while (pending.length > 0) {
    const id = pending.pop()
    if (!id || reachable.has(id)) continue
    reachable.add(id)
    const info = pluginContext.getModuleInfo(rawIdsByNormalizedId.get(id))
    const snapshotAssets = (state.imports.get(id) ?? [])
      .map((item) => item.resolvedId)
      .filter((target) => state.assets.has(target) || state.rawModules.has(target))
    for (const dependency of [...(info?.importedIds ?? []), ...(info?.dynamicallyImportedIds ?? []), ...snapshotAssets]) {
      const normalized = normalizeModuleId(dependency)
      if (tracked.has(normalized) && !reachable.has(normalized)) pending.push(normalized)
    }
  }

  return allIds.filter((id) => reachable.has(id))
}


async function captureResolvedImports(pluginContext, state, id, code) {
  const normalized = normalizeModuleId(id)
  if (!shouldTrackModule(normalized)) return

  const imports = []
  const seen = new Set()
  const sources = [state.original.get(normalized), code].filter((source) => source !== undefined)
  for (const source of sources) {
    for (const imported of collectImportSpecifiers(source)) {
      const key = `${imported.kind}\0${imported.specifier}`
      if (seen.has(key)) continue
      seen.add(key)
      imports.push(imported)
    }
  }

  const resolvedImports = []
  for (const source of imports) {
    let resolvedId = null
    try {
      const resolved = await pluginContext.resolve(source.specifier, id, { skipSelf: true })
      if (resolved && !resolved.external) resolvedId = normalizeModuleId(resolved.id)
    } catch {
      resolvedId = null
    }
    resolvedImports.push({
      ...source,
      resolvedId,
      tracked: resolvedId ? shouldTrackModule(resolvedId) : false,
    })
  }
  state.imports.set(normalized, resolvedImports)
}

function collectImportSpecifiers(code) {
  const imports = []
  const seen = new Set()
  collectImportMatches(imports, seen, code, STATIC_IMPORT_RE, 'import')
  collectImportMatches(imports, seen, code, EXPORT_FROM_RE, 'export')
  collectImportMatches(imports, seen, code, DYNAMIC_IMPORT_RE, 'dynamic-import')
  return imports
}

function collectImportMatches(imports, seen, code, pattern, kind) {
  pattern.lastIndex = 0
  let match
  while ((match = pattern.exec(code))) {
    const specifier = match[1]
    const key = `${kind}\0${specifier}`
    if (seen.has(key)) continue
    seen.add(key)
    imports.push({ kind, specifier })
  }
}

function writeSnapshot(state, id, stage, code) {
  if (code === undefined) return null
  const cleanExt = path.extname(id) || '.js'
  const safeExt = JS_LIKE_RE.test(`x${cleanExt}`) ? cleanExt : '.js'
  const name = `${hashId(id)}.${stage}${safeExt}`
  const abs = path.join(state.outDir, 'sources', name)
  fs.writeFileSync(abs, code)
  return path.relative(state.outDir, abs)
}

function normalizeSnapshotSourceForGeatsc(code) {
  if (code === undefined) return undefined
  // The vite-plugin leaves keep-alive statements so Rollup preserves child
  // component classes long enough for IR extraction. The module-graph path
  // compiles snapshots directly, so remove the bundler-only marker before
  // geatsc turns it into native constructor/prototype materialization.
  let next = code.replace(/^.*__GEA_IR_KEEP__.*$\n?/gm, '')
  next = next.replace(/\bvar (_tpl\d+_root) = null;/g, '/** @type {Node} */ var $1 = null;')
  next = next.replace(
    /\((_tpl\d+_root) \|\| \(\1 = (_tpl\d+_create)\(\)\)\)\.cloneNode\(true\)/g,
    '($1 = $1 || $2(), $1).cloneNode(true)',
  )
  return next
}

function readFileSource(id) {
  if (!path.isAbsolute(id) || !fs.existsSync(id)) return undefined
  try {
    return fs.readFileSync(id, 'utf8')
  } catch {
    return undefined
  }
}

function normalizeIdList(ids) {
  return ids.map((id) => normalizeModuleId(id)).filter((id) => shouldTrackModule(id)).sort()
}

// The gea vite plugin serves its compiler runtime from a VIRTUAL module
// (`virtual:gea-compiler-runtime`, resolved to the rollup-private `\0`-prefixed
// id). Every compiled JSX module imports its component bases and DOM binding
// helpers from it — `CompiledStaticComponent`, `GEA_STATIC_TEMPLATE`,
// `delegateEvent`, `reactiveValueRead` — so dropping it from the graph, as a
// blanket `\0` filter does, hands geatsc a program whose class heritage,
// template-method parameters and template roots all resolve to nothing. The
// symptoms are the downstream fail-closed guards, one per compile:
// "Source class heritage … effectful-or-dynamic-base-expression",
// "representation-plan coverage gap for Parameter … text=\"d\"", and
// "… for VariableDeclaration … text=\"_tpl0_root = null\"".
//
// Give those modules a synthetic absolute path instead. It never exists on
// disk, and it does not need to: the manifest carries the snapshot geatsc
// actually reads (compiler/module-graph.ts installs `sourceTextByFile` over the
// TS compiler host), and a path is what the rest of this file — id
// normalization, reachability, snapshot naming, and the specifier -> file map
// geatsc resolves imports through — is written against.
//
// Scoped to `virtual:gea-*` deliberately: rollup and vite use the same `\0`
// space for internal helper modules (preload, commonjs shims) that are bundler
// mechanics, not application source.
const GEA_VIRTUAL_MODULE_PREFIX = '\0virtual:gea-'
const GEA_VIRTUAL_MODULE_ROOT = '/@gea-virtual'

function normalizeModuleId(id) {
  if (!id) return id
  if (id.startsWith(GEA_VIRTUAL_MODULE_PREFIX)) {
    const name = id.slice(1).replace(/[?#].*$/, '').replace(/[^A-Za-z0-9_.-]/g, '_')
    return path.posix.join(GEA_VIRTUAL_MODULE_ROOT, `${name}.ts`)
  }
  if (id.startsWith('\0')) return id
  const withoutQuery = id.replace(/[?#].*$/, '')
  return path.isAbsolute(withoutQuery) ? path.normalize(withoutQuery) : withoutQuery
}

function shouldTrackModule(id) {
  if (!id || id.startsWith('\0')) return false
  return JS_LIKE_RE.test(id)
}

function hashId(id) {
  return crypto.createHash('sha256').update(id).digest('hex').slice(0, 16)
}
