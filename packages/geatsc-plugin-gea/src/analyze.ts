import { analyzeWorkerModules } from './analyze-workers.js'
import { inferCanvasOnly } from './analyze-runtime.js'
import { nodeAuxVersion, addNodeAuxFeatures, addUnknownNodeAux, cssUsesNodeAttributes } from './analyze-node-aux.js'
import { sourceLiteralResolver } from './analyze-literals.js'
import fs from 'node:fs'
import { analyzeClassCapacity } from './analyze-classes.js'
import ts from 'typescript'
import path from 'node:path'
import { createRequire } from 'node:module'
import type { HostBindingAnalysisPatch } from './types.js'
import { nodeAnalysisVersion, addNodeFeatures, addUnknownNodeFeatures } from './analyze-nodes.js'
import { cssRangeObserver } from './analyze-css-ranges.js'
import { cssAnalysisVersion, cssUsageObserver, addUnknownCssFeatures } from './analyze-css.js'
import { addRendererFeatures, addUnknownRendererFeatures, rendererAnalysisVersion, rendererOcclusionAnalysisVersion, rendererVariableAnalysis } from './analyze-renderer.js'

export function capabilitiesToAnalyzePatch(capabilities: string[]): HostBindingAnalysisPatch {
  const features: string[] = []
  const bindings: string[] = []
  for (const capability of capabilities) {
    if (capability === 'https') features.push(capability)
    else bindings.push(capability)
  }
  return { bindings, features }
}

// The source scan runs at CMake configure time, before any bundle or IR
// exists, so it is the only authority the board build has for what the
// firmware must link. It therefore has to answer the IR's `hostCapabilities`
// questions from text alone: which host bindings are imported, which host
// globals are called, and whether any URL needs TLS. A miss is silent and
// expensive -- an app whose `https://` literal is not seen is built without
// the certificate bundle, and ESP-IDF 6's esp-tls then refuses every
// connection with "No server verification option set".
export function analyzeSourceHostBindings(entry: string): HostBindingAnalysisPatch {
  const bindings = new Set<string>()
  const features = new Set<string>([rendererAnalysisVersion, rendererOcclusionAnalysisVersion, cssAnalysisVersion, nodeAnalysisVersion, nodeAuxVersion])
  const discovery = discoverSourceFiles(entry)
  const classSources = new Map<string, string>()
  let classUnknown = discovery.unknown
  const variables = rendererVariableAnalysis(features)
  const literalSources = new Map([...discovery.files].filter(file => fs.existsSync(file)).map(file => [file, fs.readFileSync(file, 'utf8')]))
  const literals = sourceLiteralResolver(literalSources, discovery.unknown)
  const css = cssUsageObserver(features, literals)
  const ranges = cssRangeObserver(features)
  const observer = {
    isSourceMethod: literals.isSourceMethod,
    isString: literals.isString,
    isBoolean: literals.isBoolean,
    selector(value: string): void { css.selector?.(value); ranges.selector?.(value); if (cssUsesNodeAttributes(value)) features.add('node-attributes') },
    property(name: string | undefined, value: string | undefined, expression?: ts.Expression): void { css.property(name, value, expression); ranges.property(name, value); variables.property(name, value) },
    unknown(): void { features.add('renderer-occlusion-triangles'); classUnknown = true; css.unknown(); ranges.unknown(); variables.unknown(); addUnknownNodeFeatures(features); addUnknownNodeAux(features) },
    unknownRanges(): void { features.add('renderer-occlusion-triangles'); ranges.unknown(); css.unknownDefaults() },
    unknownCircleBounds(): void { ranges.unknownCircleBounds?.() },
  }
  if (discovery.unknown) { variables.unknown(); ranges.unknown(); addUnknownNodeFeatures(features); addUnknownNodeAux(features) }
  if (discovery.unknown) { addUnknownRendererFeatures(features); addUnknownCssFeatures(features) }
  for (const file of discovery.files) {
    if (!fs.existsSync(file)) continue
    const text = fs.readFileSync(file, 'utf8')
    classSources.set(file, text)
    addBindingsForGeaEmbeddedImports(text, bindings)
    addBindingsForEmbeddedHostNames(text, bindings)
    addBindingsForHostGlobals(text, bindings)
    addFeaturesForUrlSchemes(text, features)
    if (analyzeWorkerModules(file, text).realms) features.add('worker-realms')
    addRendererFeatures(file, text, features, observer, variables)
    addNodeFeatures(file, text, features)
    addNodeAuxFeatures(file, text, features, literals.isNumeric)
  }
  const classCapacity = analyzeClassCapacity(classSources, classUnknown || features.has('node-inputs') || features.has('node-images'))
  if (classCapacity !== undefined) {
    features.add(`node-class-capacity-v1-${classCapacity}`)
    features.add(`node-class-storage-v1-${classCapacity}`)
  }
  features.add('runtime-analysis-v1')
  if (inferCanvasOnly(literalSources, discovery.unknown || discovery.runtimeUnknown)) features.add('runtime-canvas-only')
  variables.finish()
  css.finish()
  ranges.finish()
  return { bindings: [...bindings].sort(), features: [...features].sort() }
}

export function mergeAnalyzePatches(...patches: HostBindingAnalysisPatch[]): HostBindingAnalysisPatch {
  const bindings = new Set<string>()
  const features = new Set<string>()
  for (const patch of patches) {
    for (const binding of patch.bindings ?? []) bindings.add(binding)
    for (const feature of patch.features ?? []) features.add(feature)
  }
  return {
    bindings: [...bindings].sort(),
    features: [...features].sort(),
  }
}

const importBindingNames = new Map<string, string>([
  ['Apps', 'apps'],
  ['BLE', 'ble'],
  ['BLEServer', 'ble'],
  ['WiFi', 'wifi'],
  // The device HTTP server runs on lwip; importing `http` pulls in the same
  // WiFi/network bring-up binding so the TCP/IP stack is initialized.
  ['http', 'wifi'],
  ['Accelerometer', 'imu'],
  ['Audio', 'audio'],
  ['Display', 'display'],
  ['Memory', 'memory'],
  ['Input', 'input'],
  ['audioContext', 'audio'],
  ['loadImage', 'image'],
  ['Camera', 'camera'],
  ['CameraView', 'camera'],
])

const embeddedHostNamePrefixes: Array<[prefix: string, binding: string]> = [
  ['__gea_Accelerometer', 'imu'],
  ['__gea_audioContext', 'audio'],
  ['__gea_Audio', 'audio'],
  ['__gea_Display', 'display'],
  ['__gea_Memory', 'memory'],
  ['__gea_Camera', 'camera'],
]

const namespaceImportBindings = ['apps', 'audio', 'ble', 'camera', 'display', 'image', 'imu', 'memory', 'wifi']
// Property access (`document.` / `document[`), plus VALUE positions: an
// interface cast (`document as unknown as EventSourceLike` — sky-hop-canvas
// bindInput's whole DOM usage) and argument/assignment positions. Without the
// value forms the document runtime include is dropped and every facade
// capability silently returns missing at runtime.
const documentHostRegex = /(?:\bdocument\s*(?:\.|\[)|\bwindow\s*\.\s*document\b|\bdocument\s+as\b|[=(,]\s*document\s*[),;])/

function addBindingsForGeaEmbeddedImports(text: string, bindings: Set<string>): void {
  const importRegex = /\bimport\s+([\s\S]*?)\s+from\s+['"](?:gea-embedded|@geastack\/core|@geajs\/core)['"]/g
  let match: RegExpExecArray | null
  while ((match = importRegex.exec(text)) !== null) {
    const clause = match[1]
    if (/\*\s+as\s+/.test(clause)) {
      for (const binding of namespaceImportBindings) bindings.add(binding)
      continue
    }
    const named = clause.match(/\{([\s\S]*?)\}/)
    if (!named) continue
    for (const specifier of named[1].split(',')) {
      const importedName = specifier.trim().split(/\s+as\s+/)[0]?.trim()
      if (!importedName) continue
      const binding = importBindingNames.get(importedName)
      if (binding) bindings.add(binding)
    }
  }
}

// `fetch(...)` and `new WebSocket(...)` are globals, never imports, so the
// import scan cannot see them; both need the network stack. A member call
// (`this.fetch(`, `store.fetch(`) is an app method, not the host global.
const hostGlobalCalls: Array<[pattern: RegExp, binding: string]> = [
  [/(?<![.\w$])fetch\s*\(/, 'fetch'],
  [/(?<![.\w$])new\s+WebSocket\s*\(/, 'websocket'],
  [/(?<![.\w$])new\s+(?:window\.|globalThis\.)?RTCPeerConnection\s*\(/, 'rtc'],
  [/(?<![.\w$])navigator\s*\.\s*mediaDevices\s*\.\s*getUserMedia\s*\(/, 'audio'],
  [/(?<![.\w$])new\s+(?:window\.|globalThis\.)?(?:AudioContext|PcmAudioStream)\s*\(/, 'audio'],
]

// A file that declares its own `fetch` (a class method, or a wrapper such as
// image-demo's runtime.ts) calls that one; its network use, if any, shows up
// through the URL scheme scan instead. A declaration is the only `fetch(`
// whose parameter list is followed by a body.
const fetchDeclarationRegex = /\bfunction\s+fetch\b|\bfetch\s*\([^()]*\)\s*(?::[^{;]*)?\{/

function addBindingsForHostGlobals(text: string, bindings: Set<string>): void {
  const shadowed = fetchDeclarationRegex.test(text)
  for (const [pattern, binding] of hostGlobalCalls) {
    if (binding === 'fetch' && shadowed) continue
    if (pattern.test(text)) bindings.add(binding)
  }
}

// Only a quoted `https://` counts: a bare one is almost always a comment
// citing a source, and linking mbedtls for a comment would cost every app
// that documents itself ~100 KB of flash and the TLS heap at first use.
const httpsLiteralRegex = /['"`]https:\/\//
const wssLiteralRegex = /['"`]wss:\/\//

function addFeaturesForUrlSchemes(text: string, features: Set<string>): void {
  if (httpsLiteralRegex.test(text) || wssLiteralRegex.test(text)) features.add('https')
}

function addBindingsForEmbeddedHostNames(text: string, bindings: Set<string>): void {
  for (const [prefix, binding] of embeddedHostNamePrefixes) {
    if (text.includes(prefix)) bindings.add(binding)
  }
  if (documentHostRegex.test(text)) bindings.add('dom')
}

// The aliases the native build applies to the application's imports
// (core/scripts/build-gea-vite-geatsc.mjs: resolveAppModuleAliases and
// resolveGeaThreeAliasModules, applied by gea-vite-module-graph-plugin.mjs).
// The scan must walk the program the build compiles: an app that opts into
// the port (`"gea": { "geaThreejs": true }`, as skytail does) compiles its
// `three/src/*.js` imports against @geastack/gea-threejs, and
// scanning upstream three's loaders instead read their `fetch(` as a network
// binding and linked the whole Wi-Fi/WebRTC stack into a game with no network.
interface SourceAlias {
  readonly find: RegExp | string
  readonly replacement: (match: RegExpMatchArray) => string
}

function applicationDir(entry: string): string | null {
  let dir = path.dirname(path.resolve(entry))
  for (;;) {
    if (fs.existsSync(path.join(dir, 'package.json'))) return dir
    const parent = path.dirname(dir)
    if (parent === dir) return null
    dir = parent
  }
}

function buildSourceAliases(entry: string): SourceAlias[] {
  const appDir = applicationDir(entry)
  if (!appDir) return []
  const aliases: SourceAlias[] = []
  let geaThreejs = false
  try {
    const manifest = JSON.parse(fs.readFileSync(path.join(appDir, 'package.json'), 'utf8')) as {
      gea?: { moduleAliases?: unknown; geaThreejs?: unknown }
    }
    geaThreejs = manifest.gea?.geaThreejs === true
    const raw = manifest.gea?.moduleAliases
    if (raw && typeof raw === 'object' && !Array.isArray(raw)) {
      for (const [specifier, target] of Object.entries(raw as Record<string, unknown>)) {
        if (typeof target !== 'string' || !target) continue
        const replacement = path.resolve(appDir, target)
        aliases.push({ find: specifier, replacement: () => replacement })
      }
    }
  } catch {
    // An unreadable manifest names no aliases; the build reports it.
  }
  // The three.js redirect is opt-in, exactly as in the build.
  if (!geaThreejs) return aliases
  const appRequire = createRequire(path.join(appDir, 'package.json'))
  let geaThreeSrcDir = ''
  try {
    geaThreeSrcDir = path.join(path.dirname(fs.realpathSync(appRequire.resolve('@geastack/gea-threejs/package.json'))), 'src')
  } catch {
    return aliases
  }
  aliases.push({ find: /^three$/, replacement: () => path.join(geaThreeSrcDir, 'Three.ts') })
  aliases.push({ find: /^three\/src\/(.+)\.js$/, replacement: (m) => path.join(geaThreeSrcDir, `${m[1]}.ts`) })
  try {
    appRequire.resolve('troika-three-text')
    const troika = fs.realpathSync(appRequire.resolve('@geastack/native-webgl-angle/troika-three-text'))
    aliases.push({ find: /^troika-three-text$/, replacement: () => troika })
  } catch {
    // No troika, or no native text module: the build fails on the latter itself.
  }
  return aliases
}

function aliasedModule(aliases: readonly SourceAlias[], specifier: string): string | null {
  for (const alias of aliases) {
    if (typeof alias.find === 'string') {
      if (specifier === alias.find) return alias.replacement([specifier])
      continue
    }
    const match = specifier.match(alias.find)
    if (match) return alias.replacement(match)
  }
  return null
}

function discoverSourceFiles(entry: string): { files: string[]; unknown: boolean; runtimeUnknown: boolean } {
  const visited = new Set<string>()
  const files: string[] = []
  let unknown = false
  let runtimeUnknown = false
  const aliases = buildSourceAliases(entry)

  function visit(file: string): void {
    const resolved = path.resolve(file)
    // An entry that is a directory (a configure run without app metadata)
    // is not a program; reading it would throw EISDIR out of the analyzer.
    if (visited.has(resolved) || !fs.existsSync(resolved) || !fs.statSync(resolved).isFile()) return
    visited.add(resolved)
    // Imported data is not executable source. Parsing a JPEG or JSON as TS
    // manufactures syntax errors and falsely marks every renderer feature as
    // reachable. Dynamic use of data as CSS is still observed at its call site.
    if (/\.(?:json|png|jpe?g|gif|webp|bmp|ico|ttf|otf|woff2?|wav|mp3|ogg|mp4)$/i.test(resolved)) {
      // Asset imports can generate decoder/host calls outside this source
      // graph. Plain JSON is data; other assets cannot certify a minimal boot.
      if (!/\.json$/i.test(resolved)) runtimeUnknown = true
      return
    }
    const text = fs.readFileSync(resolved, 'utf8')
    if (!/\.css$/i.test(resolved)) {
      const workers = analyzeWorkerModules(resolved, text)
      if (workers.unknown) unknown = true
      for (const specifier of workers.modules) {
        const dependency = !/^[a-z]+:/i.test(specifier) && resolveRelativeModule(resolved, specifier)
        if (dependency) visit(dependency)
        else unknown = true
      }
    }
    for (const specifier of /\.css$/i.test(resolved) ? [] : moduleSpecifiers(resolved, text)) {
      // Framework host imports do not inject renderer instructions. Other
      // external code may supply styles/components we cannot inspect here.
      if (/^(?:gea-embedded|@geastack\/(?:core|engine)|@geajs\/core)(?:\/|$)/.test(specifier)) continue
      const aliased = aliasedModule(aliases, specifier)
      const dependency = aliased ?? (specifier.startsWith('.') ? resolveRelativeModule(resolved, specifier) : resolvePackageModule(resolved, specifier))
      if (dependency) visit(dependency)
      else unknown = true
    }
    if (/\.css$/i.test(resolved)) {
      for (const match of text.replace(/\/\*[\s\S]*?\*\//g, '').matchAll(/@import\s+(?:url\(\s*(?:['"]([^'"]+)['"]|([^\s)]+))\s*\)|['"]([^'"]+)['"])/g)) {
        const specifier = match[1] ?? match[2] ?? match[3]
        const dependency = !/^(?:[a-z]+:|\/)/i.test(specifier) && resolveRelativeModule(resolved, specifier)
        if (dependency) visit(dependency)
        else unknown = true
      }
    }
    files.push(resolved)
  }

  visit(entry)
  if (!files.length) unknown = true
  return { files, unknown, runtimeUnknown }
}

function moduleSpecifiers(file: string, text: string): string[] {
  const source = ts.createSourceFile(file, text, ts.ScriptTarget.Latest, true, /\.[jt]sx$/i.test(file) ? ts.ScriptKind.TSX : ts.ScriptKind.TS)
  const specifiers: string[] = []
  // An empty specifier deliberately fails resolution, retaining all features.
  // A malformed or computed module boundary cannot establish absence of CSS.
  if ((source as ts.SourceFile & { parseDiagnostics?: readonly ts.Diagnostic[] }).parseDiagnostics?.length) specifiers.push('')
  const add = (expression: ts.Expression | undefined): void => {
    specifiers.push(expression && ts.isStringLiteralLike(expression) ? expression.text : '')
  }
  const visit = (node: ts.Node): void => {
    if ((ts.isImportDeclaration(node) || ts.isExportDeclaration(node)) && node.moduleSpecifier) add(node.moduleSpecifier)
    if (ts.isImportEqualsDeclaration(node) && ts.isExternalModuleReference(node.moduleReference)) add(node.moduleReference.expression)
    if (ts.isCallExpression(node) && (node.expression.kind === ts.SyntaxKind.ImportKeyword ||
        (ts.isIdentifier(node.expression) && node.expression.text === 'require'))) add(node.arguments[0])
    ts.forEachChild(node, visit)
  }
  visit(source)
  return specifiers
}

function resolvePackageModule(importer: string, specifier: string): string | null {
  // Installed JS libraries can be the only consumers of networking and audio
  // (e.g. livekit-client). Traverse their real source just like relative imports.
  // Unresolved/native imports still retain the conservative renderer feature set.
  try { return createRequire(importer).resolve(specifier) } catch { return null }
}

function resolveRelativeModule(importer: string, specifier: string): string | null {
  const base = path.resolve(path.dirname(importer), specifier)
  for (const candidate of moduleCandidates(base)) {
    if (fs.existsSync(candidate) && fs.statSync(candidate).isFile()) return candidate
  }
  return null
}

function moduleCandidates(base: string): string[] {
  const extensions = ['', '.ts', '.tsx', '.js', '.jsx', '.mjs', '.cjs']
  return [
    // TypeScript's NodeNext imports commonly spell the emitted .js extension.
    ...(/\.[cm]?jsx?$/.test(base) ? [base.replace(/\.js$/, '.ts'), base.replace(/\.jsx?$/, '.tsx'), base.replace(/\.mjs$/, '.mts'), base.replace(/\.cjs$/, '.cts')] : []),
    ...extensions.map((extension) => `${base}${extension}`),
    ...extensions.filter(Boolean).map((extension) => path.join(base, `index${extension}`)),
  ]
}
