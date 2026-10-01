import path from 'node:path'
import fs from 'node:fs'
import { analyzeSourceHostBindings, capabilitiesToAnalyzePatch, mergeAnalyzePatches } from './analyze.js'
import {
  generateCppIrAssetForwardDeclarations,
  generateCppIrMountedSource,
  generateCppIrNamespaceSource,
  generateCppIrSource,
  irUsesCanvas,
  collectIrConstants,
} from './cpp-ir.js'
import {
  attachTypedStorageMetadata,
  enrichConstantInitializerShapes,
  enrichNamedArrayShapesFromSource,
  enrichNamedArrayShapesFromTypeSources,
  reconcileInterfaceStructReuse,
  storeInstanceGlobalName,
  synthesizeReactiveComponentStores,
} from './cpp-stores.js'
import { inlineOnlyComponentClassesForIr, rendererReplacementsForIr } from './cpp-replacements.js'
import {
  inlineReactiveComponentGetters,
  insertReactiveRendererForwardDeclarations,
  insertReactiveSignalSupport,
  neutralizeReactiveComponentToValue,
  rewriteReactiveComponentFields,
} from './cpp-reactive-component.js'
import { decideStoreHubEmission, injectStoreSignalHub } from './cpp-store-hub.js'
import { createGeaHostShims } from './host-shims.js'
import { loadIr } from './load-ir.js'
import type { GeaIrBundleV1, GeaIrComponent, GeaIrTemplate, GeatscPlugin, HostBindingAnalysisPatch, PluginOptionMap } from './types.js'
import { sanitizeCppIdentifier } from './utils.js'
import { validateIr } from './validate.js'
import { diagnoseNonReactiveLists } from './cpp-keyed-list-diagnostics.js'
import { diagnoseBoxedKeyedComponentProps } from './cpp-keyed-component-diagnostics.js'
import { diagnoseUnmountableReactiveComponents } from './cpp-reactive-component-diagnostics.js'
import { replaceStoreMethodsFromIr, topLevelValueAccessorNamesFromSource } from './cpp-store-method-replacements.js'
import { applyTypedArrayStorage } from './cpp-store-typed-arrays.js'
import { foldCanvasColorLiteralsInCpp } from './cpp-canvas-colors.js'
import { bridgeGeneratedComponentRenderers, replaceOutOfLineGeneratedRenderers } from './cpp-source-replacements.js'

export function geatscPlugin(): GeatscPlugin {
  let ir: GeaIrBundleV1 | null = null
  let irPath: string | null = null
  let loadError: string | null = null
  let entryPath: string | null = null

  return {
    name: 'gea',
    configure(context) {
      entryPath = context.entry
      const loaded = loadIr(context.options, context.entry)
      ir = loaded.ir
      irPath = loaded.path
      loadError = loaded.error
      // EXPERIMENTAL (ReactiveComponent): a ReactiveComponent IS its own store —
      // synthesize a `selfStore` GeaIrStore from its reactive state so the ONE
      // mounted renderer resolves `this.field` / `this.method()` through the
      // ordinary store machinery; the selfStore flag swaps the leaves to typed
      // (`store->field.get()` readers, per-field Signal subscribe) and keeps the
      // dynamic store-method re-lowering away from the typed class.
      if (ir) ir.stores.push(...synthesizeReactiveComponentStores(ir.components))
      // Inline simple getters into template slots (`{this.label}` → `{'n=' +
      // this.count}`) so getter reads lower through the store-field path with
      // the getter's underlying fields as reactive deps. Before any consumer
      // reads the slots, so mountability gate, mount planner, and renderer all
      // see the same inlined template.
      if (ir) ir.components = ir.components.map(inlineReactiveComponentGetters)
      // Decide typed-storage metadata once, at IR-load time. Subsequent
      // passes (typed-array rewrite, store-method body lowering, store
      // declaration generation) read from `field.typedStorage` rather than
      // re-running the array-of-object-of-primitives predicate.
      if (ir) {
        enrichNamedArrayShapesFromTypeSources(irSourceFiles(ir), ir.stores)
        enrichConstantInitializerShapes(ir.stores, collectIrConstants(ir), irSourceFiles(ir))
        attachTypedStorageMetadata(ir.stores)
      }
      return {
        allowAny: true,
        hostShims: createGeaHostShims(),
      }
    },
    validate() {
      if (loadError) {
        return [
          {
            kind: 'unsupported',
            code: 'gea-ir',
            message: loadError,
          },
        ]
      }
      if (!ir) {
        return [
          {
            kind: 'unsupported',
            code: 'gea-ir',
            message: 'geatsc gea plugin requires --plugin-option gea.ir=<path-to-gea-ir.json>',
          },
        ]
      }
      const diagnostics = validateIr(ir, irPath ?? '<inline>')
      // Non-fatal: warn about `{x.map(...)}` lists whose source isn't a
      // reactive array-of-objects store field (they silently render nothing on
      // the embedded target).
      diagnostics.push(...diagnoseNonReactiveLists(ir))
      // Fatal: statically typed keyed-list items must not silently cross a
      // child-component boundary through gea_cpp_value when the parent falls
      // off the native renderer path.
      diagnostics.push(...diagnoseBoxedKeyedComponentProps(ir))
      // Fatal: a ReactiveComponent without a typed mounted renderer renders
      // NOTHING at runtime (silent black screen) — fail the build instead.
      diagnostics.push(...diagnoseUnmountableReactiveComponents(ir))
      return diagnostics
    },
    analyzeHostBindings(context): HostBindingAnalysisPatch {
      const loaded = ir ?? loadIr(context.options, context.entry).ir
      return mergeAnalyzePatches(
        loaded ? capabilitiesToAnalyzePatch(loaded.hostCapabilities) : {},
        analyzeSourceHostBindings(context.entry)
      )
    },
    createCppBackend() {
      return {
        transformGeneratedSources(sources, context) {
          if (!ir) return sources
          const moduleFirst = isModuleFirstCppOutput(sources)
          let workingSources = sources
          let moduleFirstCompilerRuntimeHeader: string | null = null
          let moduleFirstRuntimeGlobalsHeader: string | null = null
          let moduleFirstNeedsGeaDocumentRuntime = false
          if (moduleFirst) {
            const activeIr = ir
            const combinedSource = sources.map((source) => source.source).join('\n')
            const topLevelValueNames = topLevelValueAccessorNamesFromSource(combinedSource)
            moduleFirstCompilerRuntimeHeader = findModuleFirstCompilerRuntimeHeader(sources)
            moduleFirstRuntimeGlobalsHeader = findModuleFirstRuntimeGlobalsHeader(sources) ?? moduleFirstCompilerRuntimeHeader
            decideStoreHubEmission(combinedSource, activeIr)
            enrichNamedArrayShapesFromSource(combinedSource, activeIr.stores)
            enrichNamedArrayShapesFromTypeSources(irSourceFiles(activeIr), activeIr.stores)
            enrichConstantInitializerShapes(activeIr.stores, collectIrConstants(activeIr), irSourceFiles(activeIr))
            attachTypedStorageMetadata(activeIr.stores)
            reconcileInterfaceStructReuse(combinedSource, activeIr.stores)

            const replacements = rendererReplacementsForIr(activeIr)
            const componentsByClass = new Map(activeIr.components.map((component) => [sanitizeCppIdentifier(component.exportName), component]))
            const removableClassNames = new Set(
              replacements
                .filter((replacement) => {
                  if ((replacement.refFields ?? []).length > 0) return false
                  const component = componentsByClass.get(replacement.className)
                  if (component?.reactiveState) return false
                  return !component || !componentHasRuntimeLifecycle(component)
                })
                .map((replacement) => replacement.className),
            )
            const includeDomInterop =
              activeIr.hostCapabilities.includes('dom') ||
              replacements.some((replacement) => !removableClassNames.has(replacement.className)) ||
              irUsesCanvas(activeIr, context.modules)
            moduleFirstNeedsGeaDocumentRuntime = needsGeaDocumentRuntime(activeIr, entryPath)
            const typedStoreAccessorTypes = typedStoreAccessorTypesForIr(activeIr)
            mergeTypedStoreAccessorTypes(typedStoreAccessorTypes, typedStoreAccessorTypesFromSource(combinedSource))
            const supportSource = generateModuleFirstGeaIrHeader(activeIr, irPath, context, {
              includeDomInterop,
              includeGeaDocumentRuntime: moduleFirstNeedsGeaDocumentRuntime,
              pluginOptions: context.options,
            })
            const typedGlobalAccessors = typedGlobalAccessorsFromAccessorTypes(typedStoreAccessorTypes)
            const mountedRenderers = generateCppIrMountedSource(activeIr, typedGlobalAccessors)
            const rendererSplit = mountedRenderers ? splitModuleFirstMountedRenderers(activeIr, mountedRenderers) : null
            const nativeBridgeFiles = new Set<string>()
            const nativeStubFiles = nativeReplacedModuleFiles(activeIr, rendererSplit, context.modules, nativeBridgeFiles)
            const rendererReplacements = rendererReplacementsForIr(activeIr)
            const replacementByClass = new Map(rendererReplacements.map((replacement) => [replacement.className, replacement]))
            const componentClassesByModule = new Map<string, string[]>()
            for (const component of activeIr.components) {
              const file = path.resolve(component.module)
              const classes = componentClassesByModule.get(file)
              const className = sanitizeCppIdentifier(component.exportName)
              if (classes) classes.push(className)
              else componentClassesByModule.set(file, [className])
            }
            const sourceFileByHeader = invertMap(moduleHeaderFileBySourceFile(context.modules))
            workingSources = [
              ...appendMountedRenderersToModuleSources(
                stubNativeReplacedModuleSources(
                  workingSources.map((source) => {
                    const transformed = transformModuleFirstGeneratedSource(
                      source,
                      activeIr,
                      moduleFirstCompilerRuntimeHeader,
                      moduleFirstRuntimeGlobalsHeader,
                      context.options,
                      topLevelValueNames,
                    )
                    // On the module-first split the class declaration lives in
                    // the module header while its method BODIES are emitted
                    // out-of-line in the sibling .cpp. Resolve that .cpp back to
                    // the same source module so the boxed template body it holds
                    // is rewritten too — the header alone carries only the
                    // declaration, so bridging just the header left the boxed
                    // renderer fully intact.
                    const isModuleCpp = source.fileName.endsWith('.cpp')
                    const sourceFile =
                      sourceFileByHeader.get(source.fileName) ??
                      (isModuleCpp ? sourceFileByHeader.get(source.fileName.replace(/\.cpp$/, '.hpp')) : undefined)
                    if (!sourceFile || !nativeBridgeFiles.has(sourceFile) || !(source.fileName.endsWith('.hpp') || isModuleCpp)) {
                      return transformed
                    }
                    const bridgeReplacements = (componentClassesByModule.get(sourceFile) ?? [])
                      .map((className) => replacementByClass.get(className))
                      .filter((replacement): replacement is NonNullable<typeof replacement> => !!replacement)
                    if (bridgeReplacements.length === 0) return transformed
                    // The .cpp holds no class body, so the member-inserting
                    // bridge must not run there — only the out-of-line rewrite.
                    if (isModuleCpp) {
                      return {
                        ...transformed,
                        source: replaceOutOfLineGeneratedRenderers(transformed.source, bridgeReplacements),
                      }
                    }
                    return {
                      ...transformed,
                      source: bridgeGeneratedComponentRenderers(
                        transformed.source,
                        bridgeReplacements,
                        rendererSplit?.headerSource ?? '',
                        typedStoreAccessorTypes,
                      ),
                    }
                  }),
                  context.modules,
                  nativeStubFiles,
                  activeIr,
                ),
                context.modules,
                rendererSplit,
                typedGlobalAccessors,
              ).map((source) => removeNativeStubTemplateCacheResets(source, nativeStubFiles)),
              {
                fileName: 'modules/gea_ir.hpp',
                source: includeRecordAliasTypeHeaders(
                  appendRendererSharedSourceToGeaIrHeader(supportSource, rendererSplit?.headerSource ?? ''),
                  sources,
                ),
              },
            ]
          }
          let externalizedDrainMicrotasks = false
          const nextSources = workingSources
            .map((source) => {
              if (source.fileName.endsWith('.hpp')) {
                const drained = externalizeDrainMicrotasksDeclaration(source.source, microtasksNamespace(context.options))
                if (drained !== source.source) externalizedDrainMicrotasks = true
                const next = stripInlineFromHeaderFunctionPrototypes(drained)
                return next === source.source ? source : { ...source, source: next }
              }
              return source
            })
          const needsDrainMicrotasksRuntime =
            externalizedDrainMicrotasks &&
            !nextSources.some((source) => source.fileName.endsWith('.cpp') && /\bvoid\s+drainMicrotasks\s*\(\s*\)\s*\{/.test(source.source))
          return ensureModuleFirstPluginRuntimeSource(nextSources, {
            includeGeaDocumentRuntime: moduleFirstNeedsGeaDocumentRuntime,
            defineDrainMicrotasks: needsDrainMicrotasksRuntime,
            microtasksNamespace: microtasksNamespace(context.options),
          })
        },
      }
    },
  }
}

export default geatscPlugin

type GeneratedCppSource = { fileName: string; source: string }

function isModuleFirstCppOutput(sources: readonly GeneratedCppSource[]): boolean {
  return (
    sources.some((source) => source.fileName === 'entry.cpp') &&
    sources.some((source) => source.fileName === 'generated_support.hpp') &&
    sources.some((source) => /^modules\/.+\.cpp$/.test(source.fileName))
  )
}

function includeModuleFirstGeaIrHeader(source: GeneratedCppSource): GeneratedCppSource {
  if (!/^modules\/.+\.(?:h|hpp)$/.test(source.fileName)) return source
  if (source.fileName.endsWith('.types.hpp')) return source
  if (source.fileName === 'modules/gea_ir.hpp') return source
  if (source.source.includes('#include "./gea_ir.hpp"')) return source
  const includeLine = '#include "./gea_ir.hpp"'
  return { ...source, source: insertModuleHeaderInclude(source.source, includeLine) }
}

function findModuleFirstCompilerRuntimeHeader(sources: readonly GeneratedCppSource[]): string | null {
  const source = sources.find((candidate) =>
    /^modules\/.+\.(?:h|hpp)$/.test(candidate.fileName) &&
    /\bclass\s+Store\s*(?::[^{]+)?\{/.test(candidate.source)
  )
  return source?.fileName ?? null
}

function findModuleFirstRuntimeGlobalsHeader(sources: readonly GeneratedCppSource[]): string | null {
  const source = sources.find((candidate) =>
    /^modules\/.+\.(?:h|hpp)$/.test(candidate.fileName) &&
    /\b__gea_global_(?:xe|Se|Ce|we|Te|De|Oe|je|Me|Ne|Pe|Fe)\s*\(\s*\)/.test(candidate.source)
  )
  return source?.fileName ?? null
}

function includeModuleFirstCompilerRuntimeHeader(
  source: GeneratedCppSource,
  compilerRuntimeHeader: string | null,
): GeneratedCppSource {
  if (!compilerRuntimeHeader) return source
  if (!/^modules\/.+\.(?:h|hpp)$/.test(source.fileName)) return source
  if (source.fileName.endsWith('.types.hpp')) return source
  if (source.fileName === compilerRuntimeHeader) return source
  if (!/:\s*public\s+Store\b/.test(source.source)) return source
  const headerBase = compilerRuntimeHeader.slice(compilerRuntimeHeader.lastIndexOf('/') + 1)
  const includeLine = `#include "./${headerBase}"`
  if (source.source.includes(includeLine)) return source
  return { ...source, source: insertModuleHeaderInclude(source.source, includeLine) }
}

function transformModuleFirstGeneratedSource(
  source: GeneratedCppSource,
  ir: GeaIrBundleV1,
  compilerRuntimeHeader: string | null,
  runtimeGlobalsHeader: string | null,
  options: PluginOptionMap,
  topLevelValueNames: ReadonlyMap<string, string>,
): GeneratedCppSource {
  let nextSource = includeModuleFirstGeaIrHeader(source)
  nextSource = includeModuleFirstCompilerRuntimeHeader(nextSource, compilerRuntimeHeader)
  if (!/\.(?:cpp|cxx|cc|h|hpp|hxx)$/.test(nextSource.fileName)) return nextSource
  if (nextSource.fileName === 'modules/gea_ir.hpp') return nextSource
  if (nextSource.fileName.endsWith('.types.hpp')) return nextSource
  let next = nextSource.source
  if (options['gea.store-relowering'] !== 'false') next = replaceStoreMethodsFromIr(next, ir, collectIrConstants(ir), topLevelValueNames)
  next = applyTypedArrayStorage(next, ir)
  next = foldCanvasColorLiteralsInCpp(next, { pixelPanelEndian: pixelPanelEndian(options) })
  const beforeRuntimeGlobals = next
  next = replaceRuntimeGlobalAccessors(next)
  next = replaceBundledCompilerRuntimeGlobalAccessors(next)
  if (next !== beforeRuntimeGlobals && runtimeGlobalsHeader && nextSource.fileName !== runtimeGlobalsHeader) {
    const headerBase = runtimeGlobalsHeader.slice(runtimeGlobalsHeader.lastIndexOf('/') + 1)
    next = insertModuleHeaderInclude(next, `#include "./${headerBase}"`)
  }
  next = replaceComponentElAccessors(next)
  next = rewriteModuleLocalTemplateBuilderCalls(next)
  next = rewriteReactiveComponentFields(next, ir.components)
  next = neutralizeReactiveComponentToValue(next, ir.components)
  next = insertReactiveSignalSupport(next, ir.components)
  next = insertReactiveRendererForwardDeclarations(next, ir.components)
  next = stripExternalInlineFromMountedRenderers(next)
  next = injectStoreSignalHub(next, ir)
  return next === nextSource.source ? nextSource : { ...nextSource, source: next }
}

function generateModuleFirstGeaIrHeader(
  ir: GeaIrBundleV1,
  irPath: string | null,
  context: { modules: Parameters<typeof generateCppIrSource>[2] },
  options: {
    includeDomInterop: boolean
    includeGeaDocumentRuntime: boolean
    pluginOptions: PluginOptionMap
  },
): string {
  const usesReactiveRuntime = ir.stores.length > 0
  return [
    '#pragma once',
    '#include "../generated_support.hpp"',
    usesReactiveRuntime ? '#include "gea/reactive_runtime.h"' : '',
    generateCppIrAssetForwardDeclarations(ir),
    generateCppIrSource(ir, irPath, context.modules, microtasksNamespace(options.pluginOptions), true),
    options.includeGeaDocumentRuntime ? generateHostDocumentNodeValueDeclaration() : '',
    cppPreludeSource(options.pluginOptions),
    generateCppIrNamespaceSource(ir, context.modules, { includeDomInterop: options.includeDomInterop, directCanvas: true }),
  ].filter(Boolean).join('\n')
}

function insertModuleHeaderInclude(source: string, includeLine: string): string {
  if (source.includes(includeLine)) return source
  const lines = source.split('\n')
  let insertAt = 0
  while (
    insertAt < lines.length &&
    (lines[insertAt].startsWith('#pragma ') || lines[insertAt].startsWith('#include ') || lines[insertAt].trim() === '')
  ) {
    insertAt++
  }
  lines.splice(insertAt, 0, includeLine)
  return lines.join('\n')
}

function stripExternalInlineFromMountedRenderers(source: string): string {
  return source.replace(
    /(^|\n)inline\s+(gea::embedded::ui::NodeHandle\s+mount_[A-Za-z_][A-Za-z0-9_]*\s*\()/g,
    '$1$2',
  )
}

function stripInlineFromHeaderFunctionPrototypes(source: string): string {
  const lines = source.split('\n')
  const out: string[] = []
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i]
    if (!line.startsWith('inline ')) {
      out.push(line)
      continue
    }
    // A line ending in `;` is a DECLARATION even when it contains `{` — a
    // default argument like `= std::vector<gea_cpp_value>{}` embeds braces
    // that must NOT be mistaken for a function body. Check `;` (declaration)
    // BEFORE `{` (definition); the reverse order left `inline` on prototypes
    // whose definition lives in the .cpp → undefined-inline link failures
    // (three.js utils.js warn/error, defaulted `params` vectors).
    const chunk = [line]
    let sawDeclaration = /;\s*$/.test(line)
    let sawDefinition = !sawDeclaration && line.includes('{')
    let cursor = i
    while (!sawDefinition && !sawDeclaration && cursor + 1 < lines.length) {
      cursor++
      chunk.push(lines[cursor])
      sawDeclaration = /;\s*$/.test(lines[cursor])
      sawDefinition = !sawDeclaration && lines[cursor].includes('{')
    }
    const joined = chunk.join(' ').replace(/^inline\s+/, '')
    const isFunctionPrototype =
      sawDeclaration && !sawDefinition && /\(/.test(joined) && /\)\s*(?:const\s*)?(?:->[^;]*)?;\s*$/.test(joined)
    if (isFunctionPrototype) {
      out.push(line.replace(/^inline\s+/, ''))
      for (let j = i + 1; j <= cursor; j++) out.push(lines[j])
      i = cursor
      continue
    }
    out.push(line)
  }
  return out.join('\n')
}

interface ModuleFirstMountedRendererSplit {
  headerSource: string
  definitionsBySourceFile: Map<string, string[]>
  sectionsBySourceFile: Map<string, string[]>
  renderedComponentIds: Set<string>
}

const MOUNTED_RENDERER_BEGIN_PREFIX = '// @geastack/geatsc-plugin-gea mounted renderer begin '
const MOUNTED_RENDERER_END = '// @geastack/geatsc-plugin-gea mounted renderer end'

function splitModuleFirstMountedRenderers(ir: GeaIrBundleV1, mountedRenderers: string): ModuleFirstMountedRendererSplit {
  const componentById = new Map(ir.components.map((component) => [component.id, component]))
  const lines = mountedRenderers.split('\n')
  const firstSection = lines.findIndex((line) => line.startsWith(MOUNTED_RENDERER_BEGIN_PREFIX))
  if (firstSection < 0) {
    return {
      headerSource: mountedRenderers.trimEnd(),
      definitionsBySourceFile: new Map(),
      sectionsBySourceFile: new Map(),
      renderedComponentIds: new Set(),
    }
  }

  const sharedLines = lines.slice(0, firstSection)
  const sharedSplit = splitMountedRendererSharedPrelude(ir, sharedLines.join('\n'))
  const sectionsBySourceFile = new Map<string, string[]>()
  const renderedComponentIds = new Set<string>()
  let index = firstSection
  while (index < lines.length) {
    const begin = lines[index]
    if (!begin.startsWith(MOUNTED_RENDERER_BEGIN_PREFIX)) {
      index += 1
      continue
    }
    let metadata: { id?: string } | null = null
    try {
      metadata = JSON.parse(begin.slice(MOUNTED_RENDERER_BEGIN_PREFIX.length))
    } catch {
      metadata = null
    }
    const end = lines.indexOf(MOUNTED_RENDERER_END, index + 1)
    if (end < 0) break
    const component = metadata?.id ? componentById.get(metadata.id) : null
    if (component) {
      renderedComponentIds.add(component.id)
      const sourceFile = path.resolve(component.module)
      const section = stripExternalInlineFromMountedRenderers(lines.slice(index, end + 1).join('\n')).trimEnd()
      if (section) {
        const existing = sectionsBySourceFile.get(sourceFile)
        if (existing) existing.push(section)
        else sectionsBySourceFile.set(sourceFile, [section])
      }
    }
    index = end + 1
  }

  return {
    headerSource: sharedSplit.headerSource,
    definitionsBySourceFile: sharedSplit.definitionsBySourceFile,
    sectionsBySourceFile,
    renderedComponentIds,
  }
}

function splitMountedRendererSharedPrelude(ir: GeaIrBundleV1, sharedSource: string): { headerSource: string; definitionsBySourceFile: Map<string, string[]> } {
  const lines = sharedSource.split('\n')
  const namespaceIndex = lines.findIndex((line) => line.trim() === 'namespace gea_ir {')
  if (namespaceIndex < 0) {
    return { headerSource: sharedSource.trimEnd(), definitionsBySourceFile: new Map() }
  }
  const prefix = lines.slice(0, namespaceIndex)
  const body = lines.slice(namespaceIndex + 1)
  const storeModuleByClass = new Map(ir.stores.map((store) => [sanitizeCppIdentifier(store.className), path.resolve(store.module)]))
  const readerForwardDecls = new Set<string>()
  const headerBody: string[] = []
  const definitionsBySourceFile = new Map<string, string[]>()
  for (let index = 0; index < body.length;) {
    const line = body[index]
    const readerMatch = /^inline\s+(.+\s+)(read_([A-Za-z_][A-Za-z0-9_]*)_field_[A-Za-z_][A-Za-z0-9_]*\s*\([^)]*\))\s*\{/.exec(line.trim())
    if (!readerMatch) {
      headerBody.push(line)
      index += 1
      continue
    }
    const sectionEnd = findCppFunctionSectionEnd(body, index)
    const storeSourceFile = storeModuleByClass.get(readerMatch[3])
    if (storeSourceFile) {
      const definition = body.slice(index, sectionEnd + 1).join('\n').replace(/^inline\s+/, '')
      const existing = definitionsBySourceFile.get(storeSourceFile)
      if (existing) existing.push(definition)
      else definitionsBySourceFile.set(storeSourceFile, [definition])
      readerForwardDecls.add(`class ${readerMatch[3]};`)
      headerBody.push(`${readerMatch[1]}${readerMatch[2]};`)
    } else {
      headerBody.push(...body.slice(index, sectionEnd + 1))
    }
    index = sectionEnd + 1
  }
  const forwardDecls = [...readerForwardDecls].filter((declaration) => !prefix.some((line) => line.trim() === declaration))
  return {
    headerSource: [...prefix, ...forwardDecls, 'namespace gea_ir {', ...headerBody, '} // namespace gea_ir'].join('\n').trimEnd(),
    definitionsBySourceFile,
  }
}

function findCppFunctionSectionEnd(lines: string[], start: number): number {
  let depth = 0
  let opened = false
  for (let index = start; index < lines.length; index += 1) {
    const line = lines[index]
    for (const char of line) {
      if (char === '{') {
        depth += 1
        opened = true
      } else if (char === '}') {
        depth -= 1
      }
    }
    if (opened && depth <= 0) return index
  }
  return start
}

function appendRendererSharedSourceToGeaIrHeader(headerSource: string, sharedSource: string): string {
  const shared = sharedSource.trim()
  if (!shared) return headerSource
  return `${headerSource.trimEnd()}\n\n${shared}\n`
}

// Module-first output puts geatsc's record-alias structs (`__gea_type_<Name>`)
// in per-module `<module>.types.hpp` headers, while the shared gea_ir.hpp can
// carry INLINE typed-item helpers (`set_typed_field(::__gea_type_X&, …)`,
// `read_number_field(const ::__gea_type_X&, …)`) whose bodies need the
// COMPLETE struct. gea_ir.hpp is included by every module header FIRST, so it
// must pull in the types.hpp that defines each record struct it references.
// (Single-file output doesn't hit this: the helper block is injected after the
// `// @geatsc record-alias types end` anchor in the same source.) The types
// headers are self-contained (#pragma once + generated_support.hpp + other
// types headers) and never include gea_ir.hpp, so no include cycle.
function includeRecordAliasTypeHeaders(headerSource: string, sources: readonly GeneratedCppSource[]): string {
  const referenced = new Set<string>()
  const referencePattern = /\b__gea_type_[A-Za-z0-9_]+\b/g
  let referenceMatch: RegExpExecArray | null
  while ((referenceMatch = referencePattern.exec(headerSource))) referenced.add(referenceMatch[0])
  if (referenced.size === 0) return headerSource
  const includes: string[] = []
  for (const source of sources) {
    if (!source.fileName.startsWith('modules/') || !source.fileName.endsWith('.types.hpp')) continue
    for (const name of referenced) {
      // Skip structs the shared header already defines itself (would #include
      // a second definition past #pragma once of a DIFFERENT file → ODR break).
      if (new RegExp(`\\bstruct\\s+${name}\\s*\\{`).test(headerSource)) continue
      if (new RegExp(`\\bstruct\\s+${name}\\s*\\{`).test(source.source)) {
        includes.push(`#include "./${source.fileName.slice('modules/'.length)}"`)
        break
      }
    }
  }
  if (includes.length === 0) return headerSource
  const anchor = '#include "../generated_support.hpp"'
  const anchorIndex = headerSource.indexOf(anchor)
  if (anchorIndex < 0) return `${includes.join('\n')}\n${headerSource}`
  const lineEnd = headerSource.indexOf('\n', anchorIndex)
  if (lineEnd < 0) return `${headerSource}\n${includes.join('\n')}\n`
  return `${headerSource.slice(0, lineEnd + 1)}${includes.join('\n')}\n${headerSource.slice(lineEnd + 1)}`
}

function appendMountedRenderersToModuleSources(
  sources: GeneratedCppSource[],
  modules: readonly { sourceFile: { fileName: string } }[],
  rendererSplit: ModuleFirstMountedRendererSplit | null,
  typedGlobalAccessors: ReadonlyMap<string, string> = new Map(),
): GeneratedCppSource[] {
  if (!rendererSplit || rendererSplit.sectionsBySourceFile.size === 0) return sources
  const cppFileBySourceFile = moduleCppFileBySourceFile(modules)
  // An IR component whose module is NOT part of the compile graph (IR built
  // from a wider vite graph, or handed in directly) still needs its renderer
  // DEFINED somewhere — dropping it leaves the gea_ir.hpp prototype dangling
  // and every mount call unresolved. Fall back to the entry module's TU
  // (last in dependency order), mirroring the single-file path that appended
  // everything to program.cpp.
  const entryCppFile =
    modules.length > 0 ? cppFileBySourceFile.get(path.resolve(modules[modules.length - 1]!.sourceFile.fileName)) : undefined
  const cppFileForSourceFile = (sourceFile: string): string | undefined =>
    cppFileBySourceFile.get(path.resolve(sourceFile)) ?? entryCppFile
  const sectionsByCppFile = new Map<string, string[]>()
  for (const [sourceFile, definitions] of rendererSplit.definitionsBySourceFile) {
    const cppFile = cppFileForSourceFile(sourceFile)
    if (!cppFile) continue
    const block = `namespace gea_ir {\n\n${definitions.join('\n\n')}\n\n} // namespace gea_ir`
    const existing = sectionsByCppFile.get(cppFile)
    if (existing) existing.push(block)
    else sectionsByCppFile.set(cppFile, [block])
  }
  for (const [sourceFile, sections] of rendererSplit.sectionsBySourceFile) {
    const cppFile = cppFileForSourceFile(sourceFile)
    if (!cppFile) continue
    const existing = sectionsByCppFile.get(cppFile)
    if (existing) existing.push(`namespace gea_ir {\n\n${sections.join('\n\n')}\n\n} // namespace gea_ir`)
    else sectionsByCppFile.set(cppFile, [`namespace gea_ir {\n\n${sections.join('\n\n')}\n\n} // namespace gea_ir`])
  }
  if (sectionsByCppFile.size === 0) return sources
  return sources.map((source) => {
    const sections = sectionsByCppFile.get(source.fileName)
    if (!sections || sections.length === 0) return source
    // The appended renderer can dereference typed globals (`__gea_global_x()->
    // method()`) whose classes the HOST module never imported in TS (e.g. a
    // controller singleton only used inside JSX event attributes geatsc
    // stripped from the class). gea_ir.hpp only forward-declares those
    // classes; member access needs the COMPLETE type, so include each
    // defining module header here (cpp → hpp, no cycle; #pragma once dedupes).
    const sectionsText = sections.join('\n')
    const includes = [
      ...accessorClassIncludesForSections(sectionsText, source, sources, typedGlobalAccessors),
      ...globalValueIncludesForSections(sectionsText, source, sources),
    ]
    const body = [
      ...(includes.length > 0 ? [includes.join('\n')] : []),
      source.source.trimEnd(),
      '',
      // Same section anchor the single-TU emission used — tooling and tests
      // locate the renderer definitions by this marker.
      '// @geastack/geatsc-plugin-gea mounted renderers',
      sections.join('\n\n'),
      '',
    ].join('\n')
    return { ...source, source: body }
  })
}

// The appended renderer can also read PLAIN VALUE globals defined by another
// module — not through an accessor, just by bare identifier. JSX style objects
// are the common case: button-tetris' Controls.tsx writes
// `style={{ left: LEFT_COL_LEFT, width: BUTTON_WIDTH }}`, where those are
// `double` globals owned by constants.tsx. geatsc strips those attributes out
// of the class body and we re-emit them here, so the host module's TS imports
// no longer explain the reference and no include is generated for it. The TU
// then fails with "'LEFT_COL_LEFT' was not declared in this scope".
//
// accessorClassIncludesForSections above covers only `__gea_global_X()`
// accessors whose type is `std::shared_ptr<Class>`; plain scalars never match
// it. Resolve them the same way: find the module layout header that DECLARES
// the identifier and include it. Layout headers are declaration-only and
// `#pragma once`, so this adds no definitions and cannot create a cycle.
function globalValueIncludesForSections(
  sectionsText: string,
  target: GeneratedCppSource,
  sources: readonly GeneratedCppSource[],
): string[] {
  const identifiers = new Set(sectionsText.match(/\b[A-Za-z_][A-Za-z0-9_]*\b/g) ?? [])
  if (identifiers.size === 0) return []

  // Include EVERY layout header that declares an identifier these sections read,
  // not just the first one that supplies each name.
  //
  // A minimal-cover variant was tried and measured: it cut the fan-out from ~9
  // extra headers per component .cpp (61 across one app) down to about one, but
  // it BROKE compilation -- button-tetris then failed with "base operand of '->'
  // has non-pointer type 'App'" in the framework's primitives header. The extra
  // headers are load-bearing for how the emitter resolves class storage, not
  // redundant duplicates, so the cheap-looking version is not safe.
  //
  // The cost is bounded by construction: these are .layout.hpp files, which the
  // emitter guarantees never include a full module wrapper (it throws if one
  // does). For button-tetris every layout header in the app totals ~119 KB, so
  // this is nothing like the re-export-barrel pathology that once turned a
  // 39-line file into a 35-minute compile by dragging in 36 façade headers.
  const includes: string[] = []
  for (const candidate of sources) {
    if (!/^modules\/.+\.layout\.hpp$/.test(candidate.fileName)) continue
    const includeLine = `#include "./${candidate.fileName.slice('modules/'.length)}"`
    if (target.source.includes(includeLine) || includes.includes(includeLine)) continue
    // `extern <type...> <name>;` -- the declaration form emitted for globals
    // whose initializer runs at module init (window.innerWidth, Math.round...),
    // which is exactly the set that cannot become a header constant.
    const externPattern = /^\s*extern\s+[^;=]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*;/gm
    let match: RegExpExecArray | null
    while ((match = externPattern.exec(candidate.source))) {
      if (!identifiers.has(match[1])) continue
      includes.push(includeLine)
      break
    }
  }
  return includes
}

function accessorClassIncludesForSections(
  sectionsText: string,
  target: GeneratedCppSource,
  sources: readonly GeneratedCppSource[],
  typedGlobalAccessors: ReadonlyMap<string, string>,
): string[] {
  if (typedGlobalAccessors.size === 0) return []
  const includes: string[] = []
  const seen = new Set<string>()
  const accessorPattern = /\b(__gea_global_[A-Za-z_][A-Za-z0-9_]*)\s*\(\s*\)/g
  let match: RegExpExecArray | null
  while ((match = accessorPattern.exec(sectionsText))) {
    if (seen.has(match[1])) continue
    seen.add(match[1])
    const accessorType = typedGlobalAccessors.get(match[1])
    const payload = accessorType ? /^std::shared_ptr<([A-Za-z_][A-Za-z0-9_]*)>$/.exec(accessorType) : null
    if (!payload) continue
    const definitionPattern = new RegExp(`\\bclass\\s+${payload[1]}\\s*(?::[^;{]*)?\\{`)
    for (const candidate of sources) {
      if (!/^modules\/.+\.hpp$/.test(candidate.fileName)) continue
      if (candidate.fileName.endsWith('.types.hpp') || candidate.fileName === 'modules/gea_ir.hpp') continue
      if (!definitionPattern.test(candidate.source)) continue
      const includeLine = `#include "./${candidate.fileName.slice('modules/'.length)}"`
      if (!target.source.includes(includeLine) && !includes.includes(includeLine)) includes.push(includeLine)
      break
    }
  }
  return includes
}

function stubNativeReplacedModuleSources(
  sources: GeneratedCppSource[],
  modules: readonly { sourceFile: { fileName: string } }[],
  stubSourceFiles: ReadonlySet<string>,
  ir: GeaIrBundleV1,
): GeneratedCppSource[] {
  if (stubSourceFiles.size === 0) return sources
  const sourceFileByCpp = invertMap(moduleCppFileBySourceFile(modules))
  const sourceFileByHeader = invertMap(moduleHeaderFileBySourceFile(modules))
  const storeHeaderIncludes = storeModuleHeaderIncludes(ir, modules)
  return sources.map((source) => {
    const headerSourceFile = sourceFileByHeader.get(source.fileName)
    if (headerSourceFile && stubSourceFiles.has(headerSourceFile)) {
      return { ...source, source: nativeStubHeaderSource(source.fileName, storeHeaderIncludes) }
    }
    const cppSourceFile = sourceFileByCpp.get(source.fileName)
    if (cppSourceFile && stubSourceFiles.has(cppSourceFile)) {
      return { ...source, source: nativeStubCppSource(source.fileName, source.source) }
    }
    return source
  })
}

function nativeStubHeaderSource(fileName: string, storeHeaderIncludes: string[]): string {
  const typeHeader = fileName.replace(/\.hpp$/, '.types.hpp').slice(fileName.lastIndexOf('/') + 1)
  return [
    '#pragma once',
    '#include "../generated_support.hpp"',
    `#include "./${typeHeader}"`,
    '#include "./gea_ir.hpp"',
    ...storeHeaderIncludes,
    '',
  ].join('\n')
}

function nativeStubCppSource(fileName: string, originalSource: string): string {
  const header = fileName.replace(/\.cpp$/, '.hpp').slice(fileName.lastIndexOf('/') + 1)
  const initMatch = /\bvoid\s+(__gea_module_\d{4})\s*\(/.exec(originalSource)
  const initName = initMatch?.[1]
  const initSource = initName ? nativeStubModuleInitSource(originalSource, initName) : null
  return [
    `#include "./${header}"`,
    '',
    ...(initName ? [initSource ?? `void ${initName}() {}`, ''] : []),
  ].join('\n')
}

function nativeStubModuleInitSource(originalSource: string, initName: string): string | null {
  const pattern = new RegExp(`\\bvoid\\s+${escapeRegExp(initName)}\\s*\\(\\s*\\)\\s*\\{`, 'm')
  const match = pattern.exec(originalSource)
  if (!match) return null
  const open = originalSource.indexOf('{', match.index)
  if (open < 0) return null
  const close = findMatchingBrace(originalSource, open)
  if (close < 0) return null
  const source = originalSource.slice(match.index, close + 1)
  return generatedMountFunctionCallPattern().test(source) ? source : null
}

function removeNativeStubTemplateCacheResets(source: GeneratedCppSource, stubSourceFiles: ReadonlySet<string>): GeneratedCppSource {
  if (stubSourceFiles.size === 0 || source.fileName !== 'entry.cpp') return source
  const next = source.source.replace(/^\s*__gea_global__tpl\d+_root(?:__gm\d+)?\(\)\s*=\s*gea_cpp_key\(gea_cpp_value::null_v\(\)\);\n/gm, '')
  return next === source.source ? source : { ...source, source: next }
}

function nativeReplacedModuleFiles(
  ir: GeaIrBundleV1,
  rendererSplit: ModuleFirstMountedRendererSplit | null,
  modules: readonly { sourceFile: { fileName: string } }[],
  bridgeFiles: Set<string> = new Set(),
): Set<string> {
  const componentFiles = nativeReplacedComponentModuleFiles(ir, rendererSplit, bridgeFiles)
  const files = new Set(componentFiles)
  if (canStubCompilerRuntimeModules(ir, componentFiles, rendererSplit)) {
    for (const module of modules) {
      const fileName = path.resolve(module.sourceFile.fileName)
      if (isExternalCompilerRuntimeModule(fileName) || isExternalRuntimeSymbolsModule(fileName)) files.add(fileName)
    }
  }
  return files
}

function nativeReplacedComponentModuleFiles(
  ir: GeaIrBundleV1,
  rendererSplit: ModuleFirstMountedRendererSplit | null,
  bridgeFiles: Set<string> = new Set(),
): Set<string> {
  const componentById = new Map(ir.components.map((component) => [component.id, component]))
  const inlineOnlyClasses = inlineOnlyComponentClassesForIr(ir)
  const files = new Set<string>()
  for (const module of ir.modules) {
    if (module.stores.length > 0 || module.components.length === 0) continue
    const components = module.components.map((id) => componentById.get(id))
    if (components.some((component) => !component)) continue
    if (components.some((component) => component?.reactiveState)) continue
    if (components.some((component) => component && componentHasRuntimeLifecycle(component))) continue
    const file = path.resolve(module.file)
    const allComponentsReplaced = components.every(
      (component) =>
        !!component &&
        (rendererSplit?.renderedComponentIds.has(component.id) ||
          inlineOnlyClasses.has(sanitizeCppIdentifier(component.exportName))),
    )
    // Stubbing is module-wide. One mounted renderer cannot justify deleting
    // sibling generated classes which a compatibility parent may instantiate.
    if (!allComponentsReplaced) continue
    files.add(file)
  }

  // A component with runtime lifecycle (onAfterRender/dispose/created) is
  // invoked through the generic render()/dispose() calling convention
  // wherever it's mounted — same-file or cross-file, the compat transform
  // emits that call shape unconditionally regardless of where the mounting
  // JSX lives. The module-wide skip above (line ~783) correctly keeps such a
  // component's OWN file out of full native replacement — a co-located
  // sibling (e.g. the page's own ReaderScreen class) may still need its
  // compat body — but that file is then simply DROPPED from `files`, so it
  // never enters the transitive-closure loop below (which only bridges files
  // that get `.delete()`d FROM `files`, i.e. files that WERE fully replaced).
  // A lifecycle component declared alongside a non-lifecycle sibling in the
  // same source file therefore got NEITHER a native stub NOR a bridge, and
  // its class was left without the render()/dispose() methods its own call
  // sites require — "no member named 'render'" (concretely: a component
  // defined in the same .tsx file as the parent component that mounts it,
  // e.g. PageArt declared inside ReaderScreen.tsx instead of its own file).
  // Seed the bridge directly for any such component that DOES have a native
  // mounted renderer available, independent of the sibling-file transitive
  // closure below.
  for (const module of ir.modules) {
    const components = module.components.map((id) => componentById.get(id)).filter((component): component is GeaIrComponent => !!component)
    for (const component of components) {
      if (component.reactiveState || !componentHasRuntimeLifecycle(component)) continue
      const className = sanitizeCppIdentifier(component.exportName)
      const hasNativeRenderer = rendererSplit?.renderedComponentIds.has(component.id) || inlineOnlyClasses.has(className)
      if (!hasNativeRenderer) continue
      bridgeFiles.add(path.resolve(module.file))
    }
  }

  // A module that remains class-backed still instantiates mounted child
  // components through the generated C++ class API. Preserve a lightweight
  // bridge class for those children instead of replacing their headers with an
  // empty stub. Propagate this constraint transitively: when a preserved
  // WeatherView mounts WeatherForecast, and WeatherForecast mounts
  // HourList/DayList, every compatibility/native boundary gets a bridge.
  //
  // Without this closure the weather graph could emit a 5-line native stub for
  // CityList while preserving CityManager's `std::make_shared<CityList>()`, which
  // fails later with an incomplete-type error. The renderer replacement itself
  // is still used for independent branches whose parents are also replaced.
  const moduleFilesByExport = new Map<string, Set<string>>()
  for (const component of ir.components) {
    const file = path.resolve(component.module)
    const existing = moduleFilesByExport.get(component.exportName)
    if (existing) existing.add(file)
    else moduleFilesByExport.set(component.exportName, new Set([file]))
  }
  let changed = true
  while (changed) {
    changed = false
    for (const component of ir.components) {
      if (files.has(path.resolve(component.module))) continue
      for (const tag of mountedComponentTags(component.template)) {
        for (const childFile of moduleFilesByExport.get(tag) ?? []) {
          if (!files.delete(childFile)) continue
          bridgeFiles.add(childFile)
          changed = true
        }
      }
    }
  }
  return files
}

function mountedComponentTags(template: GeaIrTemplate): Set<string> {
  const tags = new Set<string>()
  const pending: unknown[] = [template]
  const seen = new Set<object>()
  while (pending.length > 0) {
    const value = pending.pop()
    if (!value || typeof value !== 'object') continue
    if (seen.has(value)) continue
    seen.add(value)
    if (Array.isArray(value)) {
      for (const item of value) pending.push(item)
      continue
    }
    const record = value as Record<string, unknown>
    if (record.kind === 'mount') {
      const payload = record.payload
      if (payload && typeof payload === 'object') {
        const tag = (payload as Record<string, unknown>).tag
        if (typeof tag === 'string') tags.add(tag)
      }
    }
    for (const child of Object.values(record)) pending.push(child)
  }
  return tags
}

function canStubCompilerRuntimeModules(
  ir: GeaIrBundleV1,
  nativeReplacedComponentFiles: ReadonlySet<string>,
  rendererSplit: ModuleFirstMountedRendererSplit | null,
): boolean {
  if (ir.components.length === 0) return false
  const componentById = new Map(ir.components.map((component) => [component.id, component]))
  for (const module of ir.modules) {
    if (module.components.length === 0) continue
    if (module.stores.length > 0) return false
    const file = path.resolve(module.file)
    if (nativeReplacedComponentFiles.has(file)) continue
    const components = module.components.map((id) => componentById.get(id))
    if (
      components.length === 0 ||
      components.some((component) => !component?.reactiveState) ||
      !rendererSplit?.sectionsBySourceFile.has(file)
    ) {
      return false
    }
  }
  return true
}

function isExternalCompilerRuntimeModule(fileName: string): boolean {
  return path.basename(fileName) === 'compiler-runtime.mjs'
}

function isExternalRuntimeSymbolsModule(fileName: string): boolean {
  return path.basename(fileName) === 'symbols.mjs'
}

function moduleHeaderFileBySourceFile(modules: readonly { sourceFile: { fileName: string } }[]): Map<string, string> {
  const bySourceFile = new Map<string, string>()
  modules.forEach((module, index) => {
    const base = `${String(index).padStart(4, '0')}_${safeModuleIdentifier(moduleBaseName(module.sourceFile.fileName))}`
    bySourceFile.set(path.resolve(module.sourceFile.fileName), `modules/${base}.hpp`)
  })
  return bySourceFile
}

function invertMap(map: Map<string, string>): Map<string, string> {
  return new Map([...map.entries()].map(([key, value]) => [value, key]))
}

function storeModuleHeaderIncludes(ir: GeaIrBundleV1, modules: readonly { sourceFile: { fileName: string } }[]): string[] {
  const headerBySourceFile = moduleHeaderFileBySourceFile(modules)
  const includes = new Set<string>()
  for (const store of ir.stores) {
    if (store.selfStore) continue
    const header = headerBySourceFile.get(path.resolve(store.module))
    if (!header) continue
    includes.add(`#include "./${header.slice(header.lastIndexOf('/') + 1)}"`)
  }
  return [...includes].sort()
}

function moduleCppFileBySourceFile(modules: readonly { sourceFile: { fileName: string } }[]): Map<string, string> {
  const bySourceFile = new Map<string, string>()
  modules.forEach((module, index) => {
    const base = `${String(index).padStart(4, '0')}_${safeModuleIdentifier(moduleBaseName(module.sourceFile.fileName))}`
    bySourceFile.set(path.resolve(module.sourceFile.fileName), `modules/${base}.cpp`)
  })
  return bySourceFile
}

function moduleBaseName(fileName: string): string {
  const normalized = fileName.split('\\').join('/')
  return normalized.slice(normalized.lastIndexOf('/') + 1) || 'module'
}

function safeModuleIdentifier(value: string): string {
  return sanitizeCppIdentifier(value)
}

function cppPreludeSource(options: PluginOptionMap): string {
  const file = options['gea.cpp-prelude']
  const symbol = options['gea.cpp-prelude-symbol']
  const hasSymbol = typeof symbol === 'string' && /^[A-Za-z_][A-Za-z0-9_]*$/.test(symbol)
  if (typeof file !== 'string' || file.length === 0) return ''
  const resolved = path.resolve(file)
  const body = fs.existsSync(resolved) ? fs.readFileSync(resolved, 'utf8').trim() : ''
  if (!body && !hasSymbol) return ''
  if (hasSymbol) {
    return [
      '',
      '// @geastack/geatsc-plugin-gea C++ prelude',
      `// prelude=${resolved}`,
      `void ${symbol}() {`,
      body ? indent(body, '  ') : '',
      '}',
      '',
    ].filter((line) => line !== '').join('\n')
  }
  // The registration instance must be a C++17 `inline` variable with external
  // linkage — NOT an anonymous-namespace object. This lands in gea_ir.hpp, which
  // every module TU of a split-TU build includes; an anonymous-namespace object
  // gets one copy PER TU, so the whole static CSS tape re-registered once per
  // module (~23x on the weather app = ~3.7 MB of duplicate compiled CSS values in
  // PSRAM, fragmenting the heap until image decodes failed). The inline variable
  // is deduped by the linker to exactly one instance program-wide, so the
  // constructor (and the tape) runs once. Collisions across apps are impossible:
  // named application entry points use the mode above (--cpp-prelude-symbol), so at
  // most one object-mode prelude exists per firmware.
  return [
    '',
    '// @geastack/geatsc-plugin-gea C++ prelude',
    `// prelude=${resolved}`,
    'struct GeaPluginCppPreludeRegistration {',
    '  GeaPluginCppPreludeRegistration() {',
    indent(body, '    '),
    '  }',
    '};',
    'inline GeaPluginCppPreludeRegistration gea_plugin_cpp_prelude_registration;',
    '',
  ].join('\n')
}

function indent(source: string, prefix: string): string {
  return source.split(/\r?\n/).map((line) => `${prefix}${line}`).join('\n')
}

function needsGeaDocumentRuntime(ir: GeaIrBundleV1, entry: string | null): boolean {
  if (ir.hostCapabilities.includes('dom')) return true
  // `touch` is also a board capability. Direct canvas apps can use display
  // APIs without pulling the DOM/document event runtime.
  if (ir.components.length > 0) return true
  if (!entry) return false
  return analyzeSourceHostBindings(entry).bindings?.includes('dom') ?? false
}

function generateHostDocumentNodeValueDeclaration(): string {
  return [
    'namespace gea_ir {',
    'gea_cpp_value hostDocumentNodeValue(gea::embedded::ui::NodeHandle node, double nodeType = 1.0);',
    '}  // namespace gea_ir',
  ].join('\n')
}

function microtasksNamespace(options: PluginOptionMap): string {
  const value = options['gea.microtasks-namespace'] ?? options['microtasks-namespace']
  return typeof value === 'string' && value.length > 0 ? value : 'gea::framework::app::generated'
}

function pixelPanelEndian(options: PluginOptionMap): boolean {
  const value = options['gea.pixel-panel-endian'] ?? options['pixel-panel-endian'] ?? options.pixelPanelEndian
  return value === '1' || value === 'true' || value === 'yes'
}

function typedStoreAccessorTypesForIr(ir: GeaIrBundleV1): Map<string, string> {
  const accessors = new Map<string, string>()
  for (const store of ir.stores) {
    if (store.selfStore) continue
    const name = storeInstanceGlobalName(store)
    if (!name) continue
    accessors.set(sanitizeCppIdentifier(name), `std::shared_ptr<${sanitizeCppIdentifier(store.className)}>`)
  }
  return accessors
}

function irSourceFiles(ir: GeaIrBundleV1): string[] {
  return [ir.entry, ...ir.modules.map((module) => module.file)]
}

function typedStoreAccessorTypesFromSource(source: string): Map<string, string> {
  const accessors = new Map<string, string>()
  const pattern = /(?:^|\n)([^\n;{}()]+?)\s*&\s*__gea_global_([A-Za-z_][A-Za-z0-9_]*)\s*\(/g
  let match: RegExpExecArray | null
  while ((match = pattern.exec(source)) !== null) {
    const type = match[1].trim()
    if (type != 'gea_cpp_value') accessors.set(match[2], type)
  }
  return accessors
}

function mergeTypedStoreAccessorTypes(target: Map<string, string>, source: ReadonlyMap<string, string>): void {
  for (const [name, type] of source) target.set(name, type)
}

function typedGlobalAccessorsFromAccessorTypes(accessors: ReadonlyMap<string, string>): Map<string, string> {
  const globals = new Map<string, string>()
  for (const [name, type] of accessors) {
    globals.set(`__gea_global_${sanitizeCppIdentifier(name)}`, type)
  }
  return globals
}

function replaceRuntimeGlobalAccessors(source: string): string {
  let next = source
  for (const [pattern, replacement] of runtimeGlobalCallReplacements) {
    next = next.replace(pattern, replacement)
  }
  for (const [pattern, replacement] of runtimeGlobalValueReplacements) {
    next = next.replace(pattern, replacement)
  }
  return next
}

function replaceBundledCompilerRuntimeGlobalAccessors(source: string): string {
  let next = source
  for (const [readable, bundled] of bundledCompilerRuntimeGlobalAccessors) {
    // Only rename to the bundle's minified accessor when that accessor is
    // actually DEFINED in this program (a vite-minified bundle emits
    // `__gea_global_De` from its minified `const De = ...`). A source that
    // declares the runtime helpers under their READABLE names (unminified
    // bundles, tests) has no `De` accessor — renaming would point every call
    // site at an undefined symbol.
    if (!source.includes(bundled)) continue
    next = next.replace(new RegExp(`\\b${escapeRegExp(readable)}\\s*\\(\\s*\\)`, 'g'), `${bundled}()`)
  }
  return next
}

function rewriteModuleLocalTemplateBuilderCalls(source: string): string {
  const localTemplateBuilders = new Map<string, string>()
  for (const match of source.matchAll(/\b(fn___gea_mod_[A-Za-z0-9_]+_x2e__(tpl\d+_create))\b/g)) {
    if (!localTemplateBuilders.has(match[2])) localTemplateBuilders.set(match[2], match[1])
  }
  if (localTemplateBuilders.size === 0) return source
  let next = source
  for (const [shortSuffix, longName] of localTemplateBuilders) {
    next = next.replace(new RegExp(`\\bfn__${escapeRegExp(shortSuffix)}\\s*\\(`, 'g'), `${longName}(`)
  }
  return next
}

function replaceComponentElAccessors(source: string): string {
  return source
    .replace(
      /\(\[&\]\(\)(?:\s*->\s*decltype\(auto\))?\s*\{\s*\(void\)this->__gea_ctor_return_value\.record_get_literal\("el"\);\s*return this->__gea_ctor_return_value\.record_get_literal\("el"\);\s*\}\)\(\)/g,
      'this->__gea_dynamic_props.record_get(gea_cpp_value(std::string("__geawebsym_gea_element")))',
    )
    .replace(
      /\(\[&\]\(\)\s*\{\s*return \(\*this\)\.el\(\);\s*\}\)\(\)/g,
      'this->__gea_dynamic_props.record_get(gea_cpp_value(std::string("__geawebsym_gea_element")))',
    )
    // Direct getter-call shape (no ctor-return slot, no IIFE): a transparent-
    // proxy component emits `this.el` as a plain `(*this).el()` call.
    .replace(
      /\(\*this\)\.el\(\)/g,
      'this->__gea_dynamic_props.record_get(gea_cpp_value(std::string("__geawebsym_gea_element")))',
    )
}

function findMatchingBrace(source: string, open: number): number {
  let depth = 0
  let quote: string | null = null
  let escaped = false
  for (let index = open; index < source.length; index += 1) {
    const char = source[index]
    if (quote) {
      if (escaped) escaped = false
      else if (char === '\\') escaped = true
      else if (char === quote) quote = null
      continue
    }
    if (char === '"' || char === "'") {
      quote = char
      continue
    }
    if (char === '{') depth += 1
    else if (char === '}') {
      depth -= 1
      if (depth === 0) return index
    }
  }
  return -1
}

const generatedMountFunctionNamePatternSource =
  '(?:fn_mount(?:_x24_\\d+)?|fn___gea_mod_[A-Za-z0-9_]+_x2e_mount(?:_x24_\\d+)?)'

function generatedMountFunctionCallPattern(): RegExp {
  return new RegExp(`\\b${generatedMountFunctionNamePatternSource}\\s*\\(`)
}

function escapeRegExp(text: string): string {
  return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
}

const runtimeGlobalCallReplacements: Array<[RegExp, string]> = [
  [/\b_isArr\s*\(/g, '__gea_global__isArr()('],
  [/\bkebab\s*\(/g, '__gea_global_kebab()('],
  [/\breactiveAttr\s*\(/g, '__gea_global_reactiveAttr()('],
  [/\breactiveClass\s*\(/g, '__gea_global_reactiveClass()('],
  [/\breactiveStyle\s*\(/g, '__gea_global_reactiveStyle()('],
  [/\breactiveText\s*\(/g, '__gea_global_reactiveText()('],
  [/\breactiveTextValue\s*\(/g, '__gea_global_reactiveTextValue()('],
  [/\b_unwrap\s*\(/g, '__gea_global__unwrap()('],
  [/\bunwrap\s*\(/g, '__gea_global_unwrap()('],
]

const runtimeGlobalValueReplacements: Array<[RegExp, string]> = [
  [/\b_priv\b/g, '__gea_global__priv()'],
  [/\b_active\b/g, '__gea_global__active()'],
  [/\b_nested\b/g, '__gea_global__nested()'],
]

const bundledCompilerRuntimeGlobalAccessors: Array<[string, string]> = [
  ['__gea_global_reactiveTextValue', '__gea_global_xe'],
  ['__gea_global_reactiveText', '__gea_global_Se'],
  ['__gea_global_reactiveAttr', '__gea_global_Ce'],
  ['__gea_global_reactiveHtml', '__gea_global_we'],
  ['__gea_global_reactiveBoolAttr', '__gea_global_Te'],
  ['__gea_global_reactiveBool', '__gea_global_Ee'],
  ['__gea_global_reactiveClass', '__gea_global_De'],
  ['__gea_global_reactiveClassName', '__gea_global_Oe'],
  ['__gea_global_kebab', '__gea_global_je'],
  ['__gea_global_reactiveStyle', '__gea_global_Me'],
  ['__gea_global_reactiveStyleProp', '__gea_global_Ne'],
  ['__gea_global_reactiveValueRead', '__gea_global_Pe'],
  ['__gea_global_reactiveValue', '__gea_global_Fe'],
]

function externalizeDrainMicrotasksDeclaration(source: string, namespaceName: string): string {
  const pattern = new RegExp(
    `(namespace\\s+${escapeRegExp(namespaceName)}\\s*\\{\\s*)(?:inline\\s+)?void\\s+drainMicrotasks\\s*\\(\\s*\\)\\s*\\{\\s*gea_cpp_drain_microtasks\\s*\\(\\s*\\)\\s*;\\s*\\}(\\s*\\}\\s*//\\s*namespace\\s+${escapeRegExp(namespaceName)})`,
    'm',
  )
  return source.replace(pattern, '$1void drainMicrotasks();$2')
}

function ensureModuleFirstPluginRuntimeSource(
  sources: GeneratedCppSource[],
  options: {
    includeGeaDocumentRuntime: boolean
    defineDrainMicrotasks: boolean
    microtasksNamespace: string
  },
): GeneratedCppSource[] {
  if (!options.includeGeaDocumentRuntime && !options.defineDrainMicrotasks) return sources

  const runtimeFileName = 'modules/gea_plugin_runtime.cpp'
  const runtimeIndex = sources.findIndex((source) => source.fileName === runtimeFileName)
  const hostDocumentBridge = options.includeGeaDocumentRuntime
    ? generateHostDocumentNodeValueDefinition()
    : ''
  const drainDefinition = options.defineDrainMicrotasks
    ? generateDrainMicrotasksRuntimeDefinition(options.microtasksNamespace)
    : ''

  if (runtimeIndex < 0) {
    const lines = ['#include "../generated_support.hpp"']
    if (options.includeGeaDocumentRuntime) lines.push('#include "../host_document_gea.cpp"')
    if (hostDocumentBridge) lines.push('', hostDocumentBridge)
    if (drainDefinition) lines.push('', drainDefinition)
    return [...sources, { fileName: runtimeFileName, source: `${lines.join('\n').trimEnd()}\n` }]
  }

  const runtime = sources[runtimeIndex]!
  let source = runtime.source
  if (options.includeGeaDocumentRuntime) {
    source = insertModuleHeaderInclude(source, '#include "../host_document_gea.cpp"')
  }
  if (hostDocumentBridge && !source.includes('hostDocumentNodeValue(')) {
    source = `${source.trimEnd()}\n\n${hostDocumentBridge}\n`
  }
  if (drainDefinition) source = `${source.trimEnd()}\n\n${drainDefinition}\n`
  if (source === runtime.source) return sources
  return sources.map((candidate, index) => index === runtimeIndex ? { ...candidate, source } : candidate)
}

function generateHostDocumentNodeValueDefinition(): string {
  return [
    'namespace gea_ir {',
    'gea_cpp_value hostDocumentNodeValue(gea::embedded::ui::NodeHandle node, double nodeType) {',
    '  return gea::runtime::host::domNodeValue(node, nodeType);',
    '}',
    '}  // namespace gea_ir',
  ].join('\n')
}

function generateDrainMicrotasksRuntimeDefinition(namespaceName: string): string {
  return [
    `namespace ${namespaceName} {`,
    'void drainMicrotasks() {',
    '  gea_cpp_drain_microtasks();',
    '}',
    `}  // namespace ${namespaceName}`,
  ].join('\n')
}

function componentHasRuntimeLifecycle(component: GeaIrBundleV1['components'][number]): boolean {
  if (!component.sourceSpan) return false
  let source = ''
  try {
    source = fs.readFileSync(component.module, 'utf8')
  } catch {
    return false
  }
  const text = source.slice(component.sourceSpan.start, component.sourceSpan.end)
  return /\b(?:created|onAfterRender|dispose)\s*\(/.test(text)
}
