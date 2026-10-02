import {
  generateMountedRendererDeclarations,
  generateMountedRenderers,
  generateMountedRendererSections,
} from "./cpp-mounted.js";
import { domStyleAccessorInteropSource } from "./cpp-ir-dom-style.js";
import { isReactiveComponentIr } from "./cpp-reactive-component.js";
import {
  storeDynamicFallbackEnabled,
  storeHubBindHelperLines,
} from "./cpp-store-hub.js";
import {
  collectStoreArraySources,
  collectStoreFields,
  collectStoreMethods,
  collectSingletonMethodsFromSourceFiles,
  enrichConstantInitializerShapes,
  generateInterfaceItemHelperForwardDeclarations,
  generateStoreDeclarations,
  generateStoreStateReaders,
  generateTypedSelfStoreReaders,
  generateTypedStoreReaderDefinitions,
  storeInstanceGlobalName,
} from "./cpp-stores.js";
import type {
  EmitModule,
  GeaIrBundleV1,
  GeaIrComponent,
  GeaIrConstant,
  GeaIrConstantObjectField,
  GeaIrConstantPrimitiveType,
} from "./types.js";
import { sanitizeCppIdentifier } from "./utils.js";
import { existsSync, readFileSync, statSync } from "node:fs";
import { dirname, resolve } from "node:path";
import ts from "typescript";

export function generateCppIrSource(
  ir: GeaIrBundleV1,
  irPath: string | null,
  modules: EmitModule[],
  microtasksNamespace = "gea::framework::app::generated",
  directCanvas = false,
): string {
  const drainNamespace = cppNamespace(microtasksNamespace);
  const usesCanvas = irUsesCanvas(ir, modules);
  const emitBoxedCanvasContext = usesCanvas && !directCanvas;
  const lines = [
    "// @geastack/geatsc-plugin-gea consumed gea IR",
    `// ir=${irPath ?? "<inline>"}`,
    `// modules=${ir.modules.length} components=${ir.components.length} stores=${ir.stores.length}`,
    `// emitted-modules=${modules.length}`,
    "",
    ...(directCanvas
      ? ['#include "direct_canvas_runtime.h"']
      : []),
    '#include "gea/embedded.h"',
    '#include "ui/tree_internal.h"',
    "#include <algorithm>",
    "#include <cmath>",
    "#include <cstdio>",
    "#include <functional>",
    "#include <memory>",
    "#include <optional>",
    "#include <string>",
    "#include <type_traits>",
    "#include <utility>",
    "#include <vector>",
    "",
    ...numberFormattingSource(),
    "",
    ...(emitBoxedCanvasContext ? canvasContextValueSource() : []),
    "",
    `namespace ${drainNamespace} {`,
    "void drainMicrotasks() {",
    "  gea_cpp_drain_microtasks();",
    "}",
    `}  // namespace ${drainNamespace}`,
    "",
  ];
  return lines.join("\n");
}

export function irUsesCanvas(
  ir: GeaIrBundleV1,
  modules?: readonly EmitModule[],
): boolean {
  if (ir.hostCapabilities.includes("canvas")) return true;
  if (
    ir.components.some((component) =>
      component.template.html.includes("<canvas"),
    )
  )
    return true;
  // A non-component app (no JSX `<canvas>` template) can still drive the canvas:
  // `const ctx = Display.ctx` or `canvas.getContext('2d')`. The IR carries no
  // signal for that, but geatsc still lowers `ctx.*` to `gea_ir::canvas*` calls,
  // so the matching definitions must be emitted or the program won't link.
  return modulesUseCanvas(modules);
}

// Source-level canvas detection for apps the IR doesn't flag — scans the emitted
// module sources for the ways a 2D context is obtained or typed. A false
// positive only emits unused inline/template helpers (dead-stripped by the C++
// compiler); a false negative is a link error, so this errs toward emitting.
const CANVAS_SOURCE_SIGNALS = [
  "Display.ctx",
  "getContext",
  "CanvasRenderingContext2D",
] as const;
function modulesUseCanvas(modules: readonly EmitModule[] | undefined): boolean {
  if (!modules) return false;
  return modules.some((module) => {
    // `sourceFile` is a real ts.SourceFile at runtime (carrying `.text`) but the
    // plugin types it narrowly; fall back to reading the file by name.
    const runtimeText = (module.sourceFile as { text?: string }).text;
    const fileName = module.sourceFile.fileName;
    const text =
      typeof runtimeText === "string"
        ? runtimeText
        : existsSync(fileName)
          ? readFileSync(fileName, "utf8")
          : "";
    return CANVAS_SOURCE_SIGNALS.some((signal) => text.includes(signal));
  });
}

function nativeDisposerSource(): string[] {
  return [
    "struct NativeDisposer : public std::enable_shared_from_this<NativeDisposer> {",
    "  std::vector<std::function<void()>> callbacks;",
    "  std::vector<std::shared_ptr<NativeDisposer>> children;",
    "",
    "  void add(std::function<void()> callback) {",
    "    callbacks.push_back(std::move(callback));",
    "  }",
    "",
    "  std::shared_ptr<NativeDisposer> child() {",
    "    auto next = std::make_shared<NativeDisposer>();",
    "    children.push_back(next);",
    "    return next;",
    "  }",
    "",
    "  void dispose() {",
    "    for (auto &child_disposer : children) child_disposer->dispose();",
    "    children.clear();",
    "    for (auto it = callbacks.rbegin(); it != callbacks.rend(); ++it) (*it)();",
    "    callbacks.clear();",
    "  }",
    "};",
  ];
}

function numberFormattingSource(): string[] {
  return [
    "namespace {",
    "inline std::string geaStoreNumberToFixed(double value, double digits) {",
    '  if (std::isnan(value)) return std::string("NaN");',
    '  if (std::isinf(value)) return value > 0.0 ? std::string("Infinity") : std::string("-Infinity");',
    "  int d = static_cast<int>(digits);",
    "  if (d < 0) d = 0;",
    "  if (d > 100) d = 100;",
    "  const double scale = std::pow(10.0, static_cast<double>(d));",
    "  const double rounded = std::round(value * scale) / scale;",
    "  char buffer[160];",
    '  std::snprintf(buffer, sizeof(buffer), "%.*f", d, rounded);',
    "  return std::string(buffer);",
    "}",
    "}  // namespace",
  ];
}

export function generateCppIrNamespaceSource(
  ir: GeaIrBundleV1,
  modules: readonly EmitModule[],
  options: { includeDomInterop?: boolean; directCanvas?: boolean } = {},
): string {
  const components = sortedComponents(ir.components);
  const constants = collectIrConstants(ir);
  enrichConstantInitializerShapes(
    ir.stores,
    constants,
    ir.modules.map((module) => module.file),
  );
  const storeArrayFields = collectStoreArraySources(ir.stores);
  const storeFields = collectStoreFields(ir.stores);
  const storeMethods = collectStoreMethods(ir.stores).concat(
    collectSingletonMethodsFromSourceFiles(
      ir.modules.map((module) => module.file),
      ir.stores,
    ),
  );
  // Reactive components use a typed `mount_<Name>(shared_ptr<Name>&, …)` renderer
  // (see cpp-reactive-component); its forward declaration names the class, so it
  // can't live in this early namespace block (which precedes `class <Name>;`).
  // It is injected after the class forward declaration in index.ts instead.
  const nonReactiveComponents = components.filter(
    (component) => !isReactiveComponentIr(component),
  );
  const mountedRendererDeclarations = generateMountedRendererDeclarations(
    nonReactiveComponents,
    storeFields,
    storeArrayFields,
    constants,
    storeMethods,
  );
  const needsTemplateRuntime =
    components.length > 0 ||
    ir.stores.length > 0 ||
    mountedRendererDeclarations.length > 0;
  // Host shims can lower library/runtime classes to typed C++ methods that use
  // gea_ir::StoreMethodBatch even when the app IR has no stores/components.
  const needsStoreRuntime = needsTemplateRuntime || modules.length > 0;
  const lines = [
    "// @geastack/geatsc-plugin-gea app namespace helpers",
    // Typed mount params in this block name the typed-carrier store classes;
    // module emission order can place geatsc's own forward declarations AFTER
    // this block, so declare them here at the same (generated-namespace)
    // scope the classes are defined in.
    ...typedCarrierStoreForwardDeclarations(ir.stores),
    "namespace gea_ir {",
    "",
    ...(needsTemplateRuntime ? nativeDisposerSource() : []),
    "",
    ...(options.includeDomInterop
      ? canvasInteropSource(irUsesCanvas(ir, modules), options.directCanvas === true)
      : []),
    // Forward-declare the interface-reuse typed-item helpers before
    // `storeRuntimeSource` so the template-dependent `set_typed_field(slot, …)`
    // call inside `storeVectorItemPropertySet` resolves by ordinary lookup (ADL
    // can't reach gea_ir when `slot` is a global `::__gea_type_<Name>`).
    ...generateInterfaceItemHelperForwardDeclarations(ir.stores),
    // Merge the global gea_cpp_key overload set (value.cpp generic + per-record
    // boxers) into gea_ir. Any gea_cpp_key overload declared inside this
    // namespace (e.g. the NodeHandle boxer apps with DOM templates emit) would
    // otherwise HIDE every global overload from the unqualified calls in
    // storeRuntimeSource — boxing a plain double store field then fails to
    // compile.
    ...(needsStoreRuntime
      ? ["using ::gea_cpp_key;", ...storeRuntimeSource()]
      : []),
    ...generateStoreDeclarations(ir.stores),
    ...generateStoreStateReaders(ir.stores),
    ...mountedRendererDeclarations,
    "}",
    "",
  ];
  return lines.join("\n");
}

function typedCarrierStoreForwardDeclarations(
  stores: GeaIrBundleV1["stores"],
): string[] {
  const declarations = [
    ...new Set(
      stores
        .filter((store) => !store.selfStore)
        .map((store) => `class ${sanitizeCppIdentifier(store.className)};`),
    ),
  ];
  return declarations.length > 0 ? [...declarations, ""] : [];
}

export function generateCppIrMountedSource(
  ir: GeaIrBundleV1,
  knownGlobalAccessorTypes: ReadonlyMap<string, string> = new Map(),
): string {
  const components = sortedComponents(ir.components);
  const constants = collectIrConstants(ir);
  enrichConstantInitializerShapes(
    ir.stores,
    constants,
    ir.modules.map((module) => module.file),
  );
  const storeArrayFields = collectStoreArraySources(ir.stores);
  const storeFields = collectStoreFields(ir.stores);
  const storeMethods = collectStoreMethods(ir.stores).concat(
    collectSingletonMethodsFromSourceFiles(
      ir.modules.map((module) => module.file),
      ir.stores,
    ),
  );
  // collectSingletonMethodsFromSourceFiles guesses `globalAccess: 'reference'`
  // for non-store singleton classes, but geatsc's class-storage strategy can
  // emit the global cell as `std::shared_ptr<Class> &__gea_global_x()`. The
  // ACTUAL accessor types scanned from the generated source are authoritative
  // — reconcile the guess so event lowering emits `->method()` behind a null
  // check instead of `.method()` on a shared_ptr (compile error).
  for (const method of storeMethods) {
    if (
      method.globalAccess !== "reference" ||
      !method.storeGlobalName ||
      method.storeGlobalName === "this"
    )
      continue;
    const accessorType = knownGlobalAccessorTypes.get(
      `__gea_global_${sanitizeCppIdentifier(method.storeGlobalName)}`,
    );
    if (accessorType?.startsWith("std::shared_ptr<"))
      method.globalAccess = "shared_ptr";
  }
  // ALL components — including self-store ReactiveComponents — flow through the
  // ONE renderer; mountedRendererForComponent picks typed field access for
  // `reactiveState` components. The typed `read_X(shared_ptr<Class>)` reader
  // overloads are emitted first (this block sits after the class definitions, so
  // they can dereference the typed fields).
  const mountedRendererSections = generateMountedRendererSections(
    components,
    storeFields,
    storeArrayFields,
    constants,
    storeMethods,
  );
  const mountedRenderers = mountedRendererSections.flatMap(
    (section) => section.lines,
  );
  const typedSelfReaders = generateTypedSelfStoreReaders(ir.stores);
  // Typed fast-path definitions for the scalar reader DECLARATIONS the early
  // block emitted — must be present whenever stores exist, even with no
  // mounted renderers, so every declared reader has its definition.
  const typedStoreReaders = generateTypedStoreReaderDefinitions(ir.stores);
  const hubBindHelper = storeHubBindHelperLines(ir);
  if (
    mountedRenderers.length === 0 &&
    typedSelfReaders.length === 0 &&
    typedStoreReaders.length === 0
  )
    return "";
  // Recognized globals carry typed cells — their forward declarations here
  // must match geatsc core's or the two declarations clash on return type.
  const globalAccessorTypes = new Map<string, string>();
  for (const [name, type] of knownGlobalAccessorTypes)
    globalAccessorTypes.set(name, type);
  for (const store of ir.stores) {
    if (store.selfStore) continue;
    const name = storeInstanceGlobalName(store);
    if (name)
      globalAccessorTypes.set(
        `__gea_global_${name}`,
        `std::shared_ptr<${sanitizeCppIdentifier(store.className)}>`,
      );
  }
  const globalDeclarations = mountedRendererGlobalAccessorDeclarations(
    mountedRenderers,
    globalAccessorTypes,
  );
  return [
    "",
    "// @geastack/geatsc-plugin-gea mounted renderers",
    ...globalAccessorClassForwardDeclarations(globalDeclarations),
    ...globalDeclarations,
    "namespace gea_ir {",
    "",
    ...hubBindHelper,
    ...typedStoreReaders,
    ...typedSelfReaders,
    ...markedMountedRendererLines(mountedRendererSections),
    "}",
    "",
  ].join("\n");
}

function markedMountedRendererLines(
  sections: ReturnType<typeof generateMountedRendererSections>,
): string[] {
  return sections.flatMap((section) => [
    `// @geastack/geatsc-plugin-gea mounted renderer begin ${JSON.stringify({
      id: section.componentId,
      exportName: section.exportName,
      functionName: section.functionName,
    })}`,
    ...section.lines,
    "// @geastack/geatsc-plugin-gea mounted renderer end",
    "",
  ]);
}

// Forward declarations for the `<img src="…">` byte-array symbols, to be
// inserted at TRUE global scope (before the program's anonymous namespace). The
// definitions live `extern "C"` at global scope (generate-gea-embedded-assets);
// `extern "C"` on a *variable* inside an anonymous namespace does NOT escape the
// internal linkage GCC gives anon-namespace members, so the reference would
// mangle to `(anonymous namespace)::gea_asset_<path>` and fail to link. Declared
// at global scope, the unqualified use inside `gea_ir` resolves out to the flat
// C symbol the definition provides.
export function generateCppIrAssetForwardDeclarations(
  ir: GeaIrBundleV1,
): string {
  const components = sortedComponents(ir.components);
  const constants = collectIrConstants(ir);
  enrichConstantInitializerShapes(
    ir.stores,
    constants,
    ir.modules.map((module) => module.file),
  );
  const storeArrayFields = collectStoreArraySources(ir.stores);
  const storeFields = collectStoreFields(ir.stores);
  const storeMethods = collectStoreMethods(ir.stores).concat(
    collectSingletonMethodsFromSourceFiles(
      ir.modules.map((module) => module.file),
      ir.stores,
    ),
  );
  const mountedRenderers = generateMountedRenderers(
    components,
    storeFields,
    storeArrayFields,
    constants,
    storeMethods,
  );
  if (mountedRenderers.length === 0) return "";
  const declarations = mountedRendererAssetDeclarations(mountedRenderers);
  if (declarations.length === 0) return "";
  return [
    "// @geastack/geatsc-plugin-gea embedded-asset symbols",
    ...declarations,
  ].join("\n");
}

function sortedComponents(
  components: readonly GeaIrComponent[],
): GeaIrComponent[] {
  return [...components].sort((left, right) =>
    componentSortKey(left).localeCompare(componentSortKey(right)),
  );
}

function componentSortKey(component: GeaIrComponent): string {
  return `${component.id}\0${component.exportName}`;
}

// Typed global accessors can name app classes (e.g. a controller singleton:
// `std::shared_ptr<VoiceNotesController> &__gea_global_voiceNotes();`) that are
// only DEFINED in a later module header — in module-first output this block
// lands in the shared gea_ir.hpp, which every module header includes FIRST, so
// the class name is undeclared at this point. A shared_ptr declaration only
// needs the class declared, so forward-declare every class a typed accessor
// names. (`__gea_type_*` record structs are complete-type dependencies handled
// via the record-alias types.hpp includes, not forward declarations.)
function globalAccessorClassForwardDeclarations(
  declarations: Iterable<string>,
): string[] {
  const forwardDeclarations = new Set<string>();
  const pattern = /^std::shared_ptr<([A-Za-z_][A-Za-z0-9_]*)>\s*&/;
  for (const declaration of declarations) {
    const match = pattern.exec(declaration);
    if (match && !match[1].startsWith("__gea_type_"))
      forwardDeclarations.add(`class ${match[1]};`);
  }
  return forwardDeclarations.size > 0
    ? [...forwardDeclarations].sort().concat("")
    : [];
}

function mountedRendererGlobalAccessorDeclarations(
  lines: string[],
  globalAccessorTypes: Map<string, string> = new Map(),
): string[] {
  // Image globals are defined as native `gea::host::GeaEmbeddedImage&`
  // accessors, so their forward declarations must carry that type — a
  // `gea_cpp_value&` declaration would ambiguate against the definition. The
  // `().id` member read (emitted only by the image-id lowering; boxed values
  // never lower to a member access) marks which globals are images.
  const imageGlobals = new Set<string>();
  const imagePattern =
    /\b(__gea_global_[A-Za-z_][A-Za-z0-9_]*)\s*\(\s*\)\.id\b/g;
  for (const line of lines) {
    let match: RegExpExecArray | null;
    while ((match = imagePattern.exec(line))) imageGlobals.add(match[1]);
  }
  const declarations = new Set<string>();
  const pattern = /\b(__gea_global_[A-Za-z_][A-Za-z0-9_]*)\s*\(\s*\)/g;
  for (const line of lines) {
    let match: RegExpExecArray | null;
    while ((match = pattern.exec(line))) {
      if (nonValueRuntimeGlobals.has(match[1])) continue;
      if (imageGlobals.has(match[1]))
        declarations.add(`gea::host::GeaEmbeddedImage &${match[1]}();`);
      else if (globalAccessorTypes.has(match[1]))
        declarations.add(
          `${globalAccessorTypes.get(match[1])} &${match[1]}();`,
        );
      else declarations.add(`gea_cpp_value &${match[1]}();`);
    }
  }
  return declarations.size > 0 ? [...declarations].sort().concat("") : [];
}

// `<img src="…">` lowers to a `gea::host::image.loadBytesPtr(gea_asset_<path>,
// gea_asset_<path>_len)` call (see imageSrcAssetLines in cpp-mounted-lowering).
// Those symbols are defined `extern "C"` at global scope by
// scripts/generate-gea-embedded-assets.mjs. We forward-declare each referenced
// one here, at namespace scope — `extern "C"` is illegal at block scope, and a
// plain C++ `extern` would bind to the enclosing (in an isolated multi-program link,
// anonymous) namespace and fail to link against the global definition.
function mountedRendererAssetDeclarations(lines: string[]): string[] {
  const symbols = new Set<string>();
  const pattern = /loadBytesPtr\(\s*(gea_asset_[A-Za-z0-9_]+)\s*,/g;
  for (const line of lines) {
    let match: RegExpExecArray | null;
    while ((match = pattern.exec(line))) {
      symbols.add(match[1]);
    }
  }
  if (symbols.size === 0) return [];
  const declarations: string[] = [];
  for (const sym of [...symbols].sort()) {
    declarations.push(`extern "C" const unsigned char ${sym}[];`);
    declarations.push(`extern "C" const unsigned long ${sym}_len;`);
  }
  return declarations.concat("");
}

const nonValueRuntimeGlobals = new Set([
  "__gea_global__isArr",
  "__gea_global__nestedCache",
  "__gea_global__priv",
  "__gea_global_kebab",
  "__gea_global_reactiveAttr",
  "__gea_global_reactiveClass",
  "__gea_global_reactiveClassName",
  "__gea_global_reactiveStyle",
  "__gea_global_reactiveText",
  "__gea_global_reactiveTextValue",
  "__gea_global_reactiveValueRead",
]);

export function collectIrConstants(
  ir: GeaIrBundleV1,
): ReadonlyMap<string, GeaIrConstant> {
  const constants = new Map<string, GeaIrConstant>();
  const pending: Array<{ name: string; expr: string }> = [];
  for (const store of ir.stores) {
    for (const constant of store.constants ?? [])
      constants.set(constant.name, constant);
  }
  const pendingFiles = ir.modules.map((module) => module.file);
  const seenFiles = new Set<string>();
  for (let index = 0; index < pendingFiles.length; index += 1) {
    const file = pendingFiles[index];
    if (seenFiles.has(file) || !existsSync(file) || !statSync(file).isFile())
      continue;
    seenFiles.add(file);
    const source = readFileSync(file, "utf8");
    pending.push(...constDeclarationsInSource(file, source));
    for (const imported of localImportFiles(file, source)) {
      if (!seenFiles.has(imported)) pendingFiles.push(imported);
    }
  }
  let changed = true;
  while (changed) {
    changed = false;
    for (const item of pending) {
      if (constants.has(item.name)) continue;
      const constant = evaluateConstantExpression(
        item.name,
        item.expr,
        constants,
      );
      if (!constant) continue;
      constants.set(item.name, constant);
      changed = true;
    }
  }
  return constants;
}

function constDeclarationsInSource(
  file: string,
  source: string,
): Array<{ name: string; expr: string }> {
  const out: Array<{ name: string; expr: string }> = [];
  const sourceFile = ts.createSourceFile(
    file,
    source,
    ts.ScriptTarget.Latest,
    true,
    file.endsWith(".tsx") ? ts.ScriptKind.TSX : ts.ScriptKind.TS,
  );
  for (const statement of sourceFile.statements) {
    if (!ts.isVariableStatement(statement)) continue;
    if ((statement.declarationList.flags & ts.NodeFlags.Const) === 0) continue;
    for (const declaration of statement.declarationList.declarations) {
      if (!ts.isIdentifier(declaration.name) || !declaration.initializer)
        continue;
      out.push({
        name: declaration.name.text,
        expr: declaration.initializer.getText(sourceFile),
      });
    }
  }
  return out;
}

function localImportFiles(file: string, source: string): string[] {
  const out: string[] = [];
  const pattern = /\bimport\b[\s\S]*?\bfrom\s+['"](\.{1,2}\/[^'"]+)['"]/g;
  let match: RegExpExecArray | null;
  while ((match = pattern.exec(source))) {
    const resolved = resolveLocalSource(file, match[1]);
    if (resolved) out.push(resolved);
  }
  return out;
}

function resolveLocalSource(
  fromFile: string,
  specifier: string,
): string | null {
  const base = resolve(dirname(fromFile), specifier);
  const candidates = [
    base,
    `${base}.ts`,
    `${base}.tsx`,
    `${base}.js`,
    `${base}.jsx`,
    resolve(base, "index.ts"),
    resolve(base, "index.tsx"),
    resolve(base, "index.js"),
    resolve(base, "index.jsx"),
  ];
  // A directory import matches the bare `base` candidate — only a file is
  // loadable; directories fall through to the index.* candidates.
  return (
    candidates.find((candidate) =>
      statSync(candidate, { throwIfNoEntry: false })?.isFile(),
    ) ?? null
  );
}

function evaluateConstantExpression(
  name: string,
  expr: string,
  constants: ReadonlyMap<string, GeaIrConstant>,
): GeaIrConstant | null {
  const text = expr.trim();
  const string = stringLiteralValue(text);
  if (string !== null) return { name, value: string, valueType: "string" };
  if (/^-?(?:\d+|\d+\.\d+|\.\d+)$/.test(text))
    return { name, value: text, valueType: "number" };
  if (text === "true" || text === "false")
    return { name, value: text, valueType: "boolean" };
  if (text === "null") return { name, value: "null", valueType: "null" };

  const objectArray = evaluateObjectArrayConstant(name, text, constants);
  if (objectArray) return objectArray;

  const substituted = text.replace(
    /\b[A-Za-z_$][A-Za-z0-9_$]*\b/g,
    (identifier) => {
      const constant = constants.get(identifier);
      return constant?.valueType === "number"
        ? `(${constant.value})`
        : identifier;
    },
  );
  if (!/^[0-9+\-*/().\s]+$/.test(substituted)) return null;
  try {
    const value = Function(`"use strict"; return (${substituted});`)();
    return typeof value === "number" && Number.isFinite(value)
      ? { name, value: String(value), valueType: "number" }
      : null;
  } catch {
    return null;
  }
}

function evaluateObjectArrayConstant(
  name: string,
  expr: string,
  constants: ReadonlyMap<string, GeaIrConstant>,
): GeaIrConstant | null {
  const source = ts.createSourceFile(
    "constant.ts",
    `(${expr});`,
    ts.ScriptTarget.Latest,
    true,
    ts.ScriptKind.TS,
  );
  const statement = source.statements[0];
  if (!statement || !ts.isExpressionStatement(statement)) return null;
  const expression = stripExpressionWrappers(statement.expression);
  if (!ts.isArrayLiteralExpression(expression)) return null;

  const rows: Array<{ fields: GeaIrConstantObjectField[] }> = [];
  for (const element of expression.elements) {
    const object = stripExpressionWrappers(element);
    if (!ts.isObjectLiteralExpression(object)) return null;
    const fields: GeaIrConstantObjectField[] = [];
    for (const property of object.properties) {
      if (!ts.isPropertyAssignment(property)) return null;
      const fieldName = objectPropertyName(property.name);
      if (!fieldName) return null;
      const value = primitiveConstantFromExpression(
        property.initializer,
        constants,
      );
      if (!value) return null;
      fields.push({ name: fieldName, ...value });
    }
    rows.push({ fields });
  }
  if (rows.length === 0)
    return { name, value: "", valueType: "object-array", items: [] };

  const fieldOrder = rows[0].fields.map((field) => field.name);
  for (const row of rows) {
    if (row.fields.length !== fieldOrder.length) return null;
    const byName = new Map(row.fields.map((field) => [field.name, field]));
    if (fieldOrder.some((fieldName) => !byName.has(fieldName))) return null;
    row.fields = fieldOrder.map((fieldName) => byName.get(fieldName)!);
  }

  return { name, value: "", valueType: "object-array", items: rows };
}

function stripExpressionWrappers(node: ts.Expression): ts.Expression {
  let current = node;
  while (
    ts.isParenthesizedExpression(current) ||
    ts.isAsExpression(current) ||
    ts.isTypeAssertionExpression(current) ||
    ts.isNonNullExpression(current) ||
    ts.isSatisfiesExpression(current)
  ) {
    current = current.expression;
  }
  return current;
}

function objectPropertyName(name: ts.PropertyName): string | null {
  if (
    ts.isIdentifier(name) ||
    ts.isStringLiteral(name) ||
    ts.isNumericLiteral(name)
  )
    return name.text;
  return null;
}

function primitiveConstantFromExpression(
  expression: ts.Expression,
  constants: ReadonlyMap<string, GeaIrConstant>,
): { value: string; valueType: GeaIrConstantPrimitiveType } | null {
  const node = stripExpressionWrappers(expression);
  if (ts.isStringLiteral(node) || ts.isNoSubstitutionTemplateLiteral(node))
    return { value: node.text, valueType: "string" };
  if (ts.isNumericLiteral(node))
    return { value: node.text, valueType: "number" };
  if (
    ts.isPrefixUnaryExpression(node) &&
    node.operator === ts.SyntaxKind.MinusToken &&
    ts.isNumericLiteral(node.operand)
  ) {
    return { value: `-${node.operand.text}`, valueType: "number" };
  }
  if (node.kind === ts.SyntaxKind.TrueKeyword)
    return { value: "true", valueType: "boolean" };
  if (node.kind === ts.SyntaxKind.FalseKeyword)
    return { value: "false", valueType: "boolean" };
  if (node.kind === ts.SyntaxKind.NullKeyword)
    return { value: "null", valueType: "null" };
  if (ts.isIdentifier(node)) {
    const constant = constants.get(node.text);
    if (!constant || constant.valueType === "object-array") return null;
    return { value: constant.value, valueType: constant.valueType };
  }
  return null;
}

function stringLiteralValue(expr: string): string | null {
  const quote = expr[0];
  if ((quote !== '"' && quote !== "'") || expr[expr.length - 1] !== quote)
    return null;
  return expr
    .slice(1, -1)
    .replace(/\\n/g, "\n")
    .replace(/\\t/g, "\t")
    .replace(/\\'/g, "'")
    .replace(/\\"/g, '"')
    .replace(/\\\\/g, "\\");
}

function canvasContextValueSource(): string[] {
  return [
    "namespace {",
    "inline double geaCanvasValueNumber(const gea_cpp_value &value) {",
    "  return gea::runtime::coerce::to_number(value);",
    "}",
    "",
    "inline int geaCanvasValueInt(const gea_cpp_value &value) {",
    "  const double number = geaCanvasValueNumber(value);",
    "  if (!std::isfinite(number)) return 0;",
    "  return static_cast<int>(number + (number >= 0.0 ? 0.5 : -0.5));",
    "}",
    "",
    "inline gea::framework::graphics::pixel::native_t geaCanvasValueRgb565(const gea_cpp_value &value) {",
    "  return gea::framework::graphics::pixel::nativeFromRrggbbaa(static_cast<std::uint32_t>(geaCanvasValueNumber(value)));",
    "}",
    "",
    "inline std::string geaCanvasValueString(const gea_cpp_value &value) {",
    "  return value.is_nullish() ? std::string() : gea_cpp_to_string(value);",
    "}",
    "",
    "inline const gea_cpp_value &geaCanvasArg(const std::vector<gea_cpp_value> &args, std::size_t index) {",
    "  static const gea_cpp_value missing = gea_cpp_value::missing();",
    "  return index < args.size() ? args[index] : missing;",
    "}",
    "",
    "inline std::vector<std::uint16_t> geaCanvasUint16Vector(const gea_cpp_value &value) {",
    "  auto items = static_cast<std::vector<gea_cpp_value>>(value);",
    "  std::vector<std::uint16_t> out;",
    "  out.reserve(items.size());",
    "  for (const auto &item : items) out.push_back(static_cast<std::uint16_t>(geaCanvasValueNumber(item)));",
    "  return out;",
    "}",
    "",
    "inline std::vector<std::int32_t> geaCanvasInt32Vector(const gea_cpp_value &value) {",
    "  auto items = static_cast<std::vector<gea_cpp_value>>(value);",
    "  std::vector<std::int32_t> out;",
    "  out.reserve(items.size());",
    "  for (const auto &item : items) out.push_back(static_cast<std::int32_t>(geaCanvasValueNumber(item)));",
    "  return out;",
    "}",
    "",
    "inline std::vector<std::uint32_t> geaCanvasUint32Vector(const gea_cpp_value &value) {",
    "  auto items = static_cast<std::vector<gea_cpp_value>>(value);",
    "  std::vector<std::uint32_t> out;",
    "  out.reserve(items.size());",
    "  for (const auto &item : items) out.push_back(static_cast<std::uint32_t>(geaCanvasValueNumber(item)));",
    "  return out;",
    "}",
    "",
    "inline std::vector<gea::framework::graphics::pixel::NativeColor> geaCanvasNativeColorVector(const gea_cpp_value &value) {",
    "  auto items = static_cast<std::vector<gea_cpp_value>>(value);",
    "  std::vector<gea::framework::graphics::pixel::NativeColor> out;",
    "  out.reserve(items.size());",
    "  for (const auto &item : items) out.emplace_back(static_cast<gea::framework::graphics::pixel::native_t>(geaCanvasValueNumber(item)));",
    "  return out;",
    "}",
    "",
    "inline int geaCanvasImageId(const gea_cpp_value &value) {",
    "  if (value.kind == gea_cpp_value::kind_t::number) return geaCanvasValueInt(value);",
    '  gea_cpp_value id = value.record_get_literal("id");',
    "  if (!id.is_nullish()) return geaCanvasValueInt(id);",
    "  return -1;",
    "}",
    "",
    "inline void geaCanvasSetFillStyle(gea::embedded::ui::CanvasRenderingContext2D &ctx, const gea_cpp_value &value) {",
    "  if (value.kind == gea_cpp_value::kind_t::number) ctx.setFillStyleRgb565(geaCanvasValueRgb565(value));",
    "  else ctx.setFillStyle(geaCanvasValueString(value));",
    "}",
    "",
    "inline void geaCanvasSetStrokeStyle(gea::embedded::ui::CanvasRenderingContext2D &ctx, const gea_cpp_value &value) {",
    "  if (value.kind == gea_cpp_value::kind_t::number) ctx.setStrokeStyleRgb565(geaCanvasValueRgb565(value));",
    "  else ctx.setStrokeStyle(geaCanvasValueString(value));",
    "}",
    "",
    "inline gea_cpp_value geaCanvasContextValue(gea::embedded::ui::CanvasRenderingContext2D ctx) {",
    "  auto state = std::make_shared<gea::embedded::ui::CanvasRenderingContext2D>(ctx);",
    "  return gea_cpp_value::object(",
    "      reinterpret_cast<std::uintptr_t>(state.get()),",
    "      [state, __methodCache = std::make_shared<std::vector<std::pair<std::string, gea_cpp_value>>>()](const std::string &key) -> gea_cpp_value {",
    "        // Memoize the bound method callables. The original accessor rebuilt a fresh",
    "        // gea_cpp_callable_args closure (a heap alloc, PSRAM-routed on esp32) on EVERY",
    "        // ctx.method() access — ~40-50 calls/frame for a canvas app turned that into",
    "        // milliseconds. Build each callable once and return the cached value thereafter.",
    "        for (auto &__e : *__methodCache) if (__e.first == key) return __e.second;",
    "        gea_cpp_value __cachedMethod = [&]() -> gea_cpp_value {",
    '        if (key == "clear") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &) -> gea_cpp_value {',
    "          state->clear();",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "clearRect") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &args) -> gea_cpp_value {',
    "          state->clearRect(geaCanvasValueInt(geaCanvasArg(args, 0)), geaCanvasValueInt(geaCanvasArg(args, 1)), geaCanvasValueInt(geaCanvasArg(args, 2)), geaCanvasValueInt(geaCanvasArg(args, 3)));",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "fillRect") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &args) -> gea_cpp_value {',
    "          state->fillRect(geaCanvasValueInt(geaCanvasArg(args, 0)), geaCanvasValueInt(geaCanvasArg(args, 1)), geaCanvasValueInt(geaCanvasArg(args, 2)), geaCanvasValueInt(geaCanvasArg(args, 3)));",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "strokeRect") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &args) -> gea_cpp_value {',
    "          state->strokeRect(geaCanvasValueInt(geaCanvasArg(args, 0)), geaCanvasValueInt(geaCanvasArg(args, 1)), geaCanvasValueInt(geaCanvasArg(args, 2)), geaCanvasValueInt(geaCanvasArg(args, 3)));",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "fillCircle") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &args) -> gea_cpp_value {',
    "          if (args.size() > 3 && !args[3].is_nullish()) geaCanvasSetFillStyle(*state, args[3]);",
    "          state->fillCircle(geaCanvasValueInt(geaCanvasArg(args, 0)), geaCanvasValueInt(geaCanvasArg(args, 1)), geaCanvasValueInt(geaCanvasArg(args, 2)));",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "strokeCircle") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &args) -> gea_cpp_value {',
    "          state->strokeCircle(geaCanvasValueInt(geaCanvasArg(args, 0)), geaCanvasValueInt(geaCanvasArg(args, 1)), geaCanvasValueInt(geaCanvasArg(args, 2)));",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "fillCircleRgb565") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &args) -> gea_cpp_value {',
    "          state->fillCircleRgb565(geaCanvasValueInt(geaCanvasArg(args, 0)), geaCanvasValueInt(geaCanvasArg(args, 1)), geaCanvasValueInt(geaCanvasArg(args, 2)), geaCanvasValueRgb565(geaCanvasArg(args, 3)));",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "fillTriangleRgb565") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &args) -> gea_cpp_value {',
    "          state->fillTriangleRgb565(geaCanvasValueInt(geaCanvasArg(args, 0)), geaCanvasValueInt(geaCanvasArg(args, 1)), geaCanvasValueInt(geaCanvasArg(args, 2)), geaCanvasValueInt(geaCanvasArg(args, 3)), geaCanvasValueInt(geaCanvasArg(args, 4)), geaCanvasValueInt(geaCanvasArg(args, 5)), geaCanvasValueRgb565(geaCanvasArg(args, 6)));",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "fillCirclesRgb565") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &args) -> gea_cpp_value {',
    "          auto xs = geaCanvasUint16Vector(geaCanvasArg(args, 0));",
    "          auto ys = geaCanvasUint16Vector(geaCanvasArg(args, 1));",
    "          auto colors = geaCanvasNativeColorVector(geaCanvasArg(args, 3));",
    "          const int count = args.size() > 4 && !args[4].is_nullish() ? geaCanvasValueInt(args[4]) : -1;",
    "          state->fillCirclesRgb565(xs, ys, geaCanvasValueInt(geaCanvasArg(args, 2)), colors, count);",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "fillCirclesRgb565Uniform") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &args) -> gea_cpp_value {',
    "          auto xs = geaCanvasUint16Vector(geaCanvasArg(args, 0));",
    "          auto ys = geaCanvasUint16Vector(geaCanvasArg(args, 1));",
    "          state->fillCirclesRgb565(xs, ys, geaCanvasValueInt(geaCanvasArg(args, 2)), static_cast<gea::framework::graphics::pixel::native_t>(geaCanvasValueNumber(geaCanvasArg(args, 3))));",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "fillTrianglesRgb565Sorted") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &args) -> gea_cpp_value {',
    "          auto x0s = geaCanvasInt32Vector(geaCanvasArg(args, 0));",
    "          auto y0s = geaCanvasInt32Vector(geaCanvasArg(args, 1));",
    "          auto x1s = geaCanvasInt32Vector(geaCanvasArg(args, 2));",
    "          auto y1s = geaCanvasInt32Vector(geaCanvasArg(args, 3));",
    "          auto x2s = geaCanvasInt32Vector(geaCanvasArg(args, 4));",
    "          auto y2s = geaCanvasInt32Vector(geaCanvasArg(args, 5));",
    "          auto colors = geaCanvasUint32Vector(geaCanvasArg(args, 6));",
    "          auto order = geaCanvasInt32Vector(geaCanvasArg(args, 7));",
    "          const int count = args.size() > 8 && !args[8].is_nullish() ? geaCanvasValueInt(args[8]) : -1;",
    "          state->fillTrianglesRgb565Sorted(x0s, y0s, x1s, y1s, x2s, y2s, colors, order, count);",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "beginPath") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &) -> gea_cpp_value {',
    "          state->beginPath();",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "arc") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &args) -> gea_cpp_value {',
    "          state->arc(geaCanvasValueNumber(geaCanvasArg(args, 0)), geaCanvasValueNumber(geaCanvasArg(args, 1)), geaCanvasValueNumber(geaCanvasArg(args, 2)), geaCanvasValueNumber(geaCanvasArg(args, 3)), geaCanvasValueNumber(geaCanvasArg(args, 4)));",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "moveTo") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &args) -> gea_cpp_value {',
    "          state->moveTo(geaCanvasValueNumber(geaCanvasArg(args, 0)), geaCanvasValueNumber(geaCanvasArg(args, 1)));",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "lineTo") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &args) -> gea_cpp_value {',
    "          state->lineTo(geaCanvasValueNumber(geaCanvasArg(args, 0)), geaCanvasValueNumber(geaCanvasArg(args, 1)));",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "closePath") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &) -> gea_cpp_value {',
    "          state->closePath();",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "fill") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &) -> gea_cpp_value {',
    "          state->fill();",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "stroke") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &) -> gea_cpp_value {',
    "          state->stroke();",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "fillText") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &args) -> gea_cpp_value {',
    "          state->fillText(geaCanvasValueString(geaCanvasArg(args, 0)), geaCanvasValueInt(geaCanvasArg(args, 1)), geaCanvasValueInt(geaCanvasArg(args, 2)));",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "drawImage") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &args) -> gea_cpp_value {',
    "          const int imageId = geaCanvasImageId(geaCanvasArg(args, 0));",
    "          if (args.size() >= 5) state->drawImage(imageId, geaCanvasValueInt(geaCanvasArg(args, 1)), geaCanvasValueInt(geaCanvasArg(args, 2)), geaCanvasValueInt(geaCanvasArg(args, 3)), geaCanvasValueInt(geaCanvasArg(args, 4)));",
    "          else state->drawImage(imageId, geaCanvasValueInt(geaCanvasArg(args, 1)), geaCanvasValueInt(geaCanvasArg(args, 2)));",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "drawImageRotated90CW") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &args) -> gea_cpp_value {',
    "          const int imageId = geaCanvasImageId(geaCanvasArg(args, 0));",
    "          state->drawImageRotated90CW(imageId, geaCanvasValueInt(geaCanvasArg(args, 1)), geaCanvasValueInt(geaCanvasArg(args, 2)), geaCanvasValueInt(geaCanvasArg(args, 3)), geaCanvasValueInt(geaCanvasArg(args, 4)));",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "drawImageTiledX") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &args) -> gea_cpp_value {',
    "          const int imageId = geaCanvasImageId(geaCanvasArg(args, 0));",
    "          state->drawImageTiledX(imageId, geaCanvasValueInt(geaCanvasArg(args, 1)), geaCanvasValueInt(geaCanvasArg(args, 2)), geaCanvasValueInt(geaCanvasArg(args, 3)));",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "flush") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &) -> gea_cpp_value {',
    "          state->flush();",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "beginBatch") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &) -> gea_cpp_value {',
    "          state->beginBatch();",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "endBatch") return gea_cpp_callable_args([state](const std::vector<gea_cpp_value> &) -> gea_cpp_value {',
    "          state->endBatch();",
    "          return gea_cpp_value::missing();",
    "        });",
    "        return gea_cpp_value::missing();",
    "        }();",
    "        if (!__cachedMethod.is_nullish()) __methodCache->emplace_back(key, __cachedMethod);",
    "        return __cachedMethod;",
    "      },",
    "      [state](const std::string &key, gea_cpp_value value) {",
    '        if (key == "fillStyle") geaCanvasSetFillStyle(*state, value);',
    '        else if (key == "strokeStyle") geaCanvasSetStrokeStyle(*state, value);',
    '        else if (key == "globalAlpha") state->setGlobalAlpha(geaCanvasValueNumber(value));',
    '        else if (key == "lineWidth") state->setLineWidth(geaCanvasValueNumber(value));',
    '        else if (key == "font") state->setFont(geaCanvasValueString(value));',
    '        else if (key == "textBaseline") state->setTextBaseline(geaCanvasValueString(value));',
    '        else if (key == "textAlign") state->setTextAlign(geaCanvasValueString(value));',
    "      },",
    "      [](const std::string &key) -> bool {",
    '        return key == "clear" || key == "clearRect" || key == "fillRect" || key == "strokeRect" || key == "fillCircle" || key == "strokeCircle" ||',
    '               key == "fillCircleRgb565" || key == "fillTriangleRgb565" || key == "fillCirclesRgb565" || key == "fillCirclesRgb565Uniform" ||',
    '               key == "fillTrianglesRgb565Sorted" || key == "beginPath" || key == "arc" || key == "moveTo" ||',
    '               key == "lineTo" || key == "closePath" || key == "fill" || key == "stroke" || key == "fillText" || key == "drawImage" || key == "drawImageRotated90CW" || key == "drawImageTiledX" ||',
    '               key == "flush" || key == "beginBatch" || key == "endBatch" || key == "fillStyle" || key == "strokeStyle" ||',
    '               key == "globalAlpha" || key == "lineWidth" || key == "font" || key == "textBaseline" || key == "textAlign";',
    "      });",
    "}",
    "}  // namespace",
    "",
    "static gea_cpp_value gea_cpp_key(const gea::embedded::ui::CanvasRenderingContext2D &ctx) {",
    "  return geaCanvasContextValue(ctx);",
    "}",
  ];
}

// The boxed reactive-apply binder. Used by the boxed mount path (e.g. a component
// whose template reads MULTIPLE global stores, which can't take the single typed
// carrier). Its companions (gea_cpp_{bind_direct,run,schedule}_reactive_apply) live
// in the always-linked cpp runtime, so this def is valid in BOTH store-codegen
// modes — emit it in each so a boxed bindReactiveApply call always has a definition.
// The keyed-list entry record helper. Keyed children (`map` with a `key`) emit
// `gea_ir::keyedEntryRecord(...)` in BOTH store-codegen modes (typed/hub and the
// boxed default), so — like bindReactiveApplyDef — its definition must be present
// in each branch or a hub-mode keyed list calls an undeclared helper.
function keyedEntryRecordDef(): string[] {
  return [
    "inline gea_cpp_value keyedEntryRecord(",
    "    gea_cpp_value key,",
    "    gea_cpp_value item,",
    "    gea_cpp_value element,",
    "    gea_cpp_value disposer,",
    "    gea_cpp_value obs) {",
    "  gea_cpp_value out = gea_cpp_value::empty_record();",
    '  out.record_set_literal("key", std::move(key));',
    '  out.record_set_literal("item", std::move(item));',
    '  out.record_set_literal("element", std::move(element));',
    '  out.record_set_literal("disposer", std::move(disposer));',
    '  out.record_set_literal("obs", std::move(obs));',
    "  return out;",
    "}",
    "",
  ];
}

function bindReactiveApplyDef(): string[] {
  return [
    "template <typename Apply>",
    "inline void bindReactiveApply(",
    "    const gea_cpp_value &store,",
    "    const gea_cpp_value &disposer,",
    "    std::initializer_list<const char *> deps,",
    "    Apply apply) {",
    "  auto apply_fn = std::make_shared<std::function<void()>>(std::move(apply));",
    "  auto pending = std::make_shared<bool>(false);",
    "  gea_cpp_run_reactive_apply(apply_fn);",
    '  auto add = gea_cpp_key(disposer.record_get_literal("add"));',
    '  auto observe = gea_cpp_key(store.record_get_literal("observe"));',
    "  if (!gea::runtime::coerce::to_boolean(observe)) return;",
    "  for (const char *dep : deps) {",
    "    auto off = gea_cpp_key(observe(std::string(dep), [apply_fn, pending]() mutable -> void {",
    "      gea_cpp_schedule_reactive_apply(apply_fn, pending);",
    "    }));",
    "    if (gea::runtime::coerce::to_boolean(add)) (void)add(off);",
    "  }",
    "  // Mirror of the hub-mode binder: an apply already queued on the microtask",
    "  // queue outlives the unsubscribe, so null the shared fn on dispose too.",
    "  if (gea::runtime::coerce::to_boolean(add)) (void)add(gea_cpp_value([apply_fn]() mutable -> gea_cpp_value {",
    "    *apply_fn = nullptr;",
    "    return gea_cpp_value::missing();",
    "  }));",
    "}",
    "",
  ];
}

function storeRuntimeSource(): string[] {
  if (!storeDynamicFallbackEnabled()) {
    return [
      "template <typename Field, typename Value>",
      "inline void storeAssignField(Field &field, const Value &value) {",
      "  if constexpr (std::is_same_v<std::decay_t<Field>, std::string>) {",
      "    if constexpr (std::is_same_v<std::decay_t<Value>, std::string>) field = value;",
      "    else if constexpr (std::is_convertible_v<Value, std::string>) field = std::string(value);",
      "    else field = std::to_string(value);",
      "  } else {",
      "    field = static_cast<Field>(value);",
      "  }",
      "}",
      "",
      "inline void storeDirectBatchBegin() {}",
      "inline void storeDirectBatchEnd() {}",
      "",
      "struct StoreMethodBatch {",
      "  StoreMethodBatch() = default;",
      "  ~StoreMethodBatch() = default;",
      "  StoreMethodBatch(const StoreMethodBatch &) = delete;",
      "  StoreMethodBatch &operator=(const StoreMethodBatch &) = delete;",
      "};",
      "",
      "template <typename Owner, typename Field, typename Value>",
      "inline Value storeTypedFieldSet(",
      "    Owner &owner,",
      "    const char *key,",
      "    Field &field,",
      "    Value value) {",
      "  storeAssignField(field, value);",
      "  if constexpr (requires { owner.__gea_hub.notify(key); }) { owner.__gea_hub.notify(key); }",
      "  return value;",
      "}",
      "",
      "template <typename Owner, typename Field, typename Value>",
      "inline Value storeTypedFieldSet(",
      "    const std::shared_ptr<Owner> &owner,",
      "    const char *key,",
      "    Field &field,",
      "    Value value) {",
      "  storeAssignField(field, value);",
      "  if (owner) owner->__gea_hub.notify(key);",
      "  return value;",
      "}",
      "",
      // Legacy reactive components (CompiledReactiveComponent) still reference
      // the non-hub read-tracker even when hub mode is active for the app's
      // typed stores. The helper is benign — provide it so those (dead-in-hub)
      // classes still compile. Only one of the hub / non-hub blocks is ever
      // emitted, so this is not a redefinition of the non-hub copy.
      "template <typename TrackRead>",
      "inline void compiledStoreTrackReadIfActive(bool active, const gea_cpp_value &store_root, const char *property_name, TrackRead track_read) {",
      "  if (active) track_read(store_root, property_name);",
      "}",
      "",
      "inline bool compiledStorePlainValue(const gea_cpp_value &value) {",
      "  const gea_cpp_value *candidate = &value;",
      "  // Protocol-owned proxy targets are immutable snapshots, so this chain cannot cycle.",
      "  while (candidate->kind == gea_cpp_value::kind_t::proxy && candidate->rare_r().proxy_target) {",
      "    candidate = candidate->rare_r().proxy_target.get();",
      "  }",
      "  if (candidate->kind == gea_cpp_value::kind_t::array_value) return candidate->rare_r().typed_array_name.empty();",
      "  if (candidate->kind != gea_cpp_value::kind_t::record) return false;",
      "  if (candidate->rare_r().prototype_names && !candidate->rare_r().prototype_names->empty()) return false;",
      "  const auto &prototype = candidate->rare_r().prototype_object;",
      "  return !prototype || prototype->kind == gea_cpp_value::kind_t::null_value ||",
      "         gea_cpp_strict_equals(*prototype, gea_cpp_object_prototype());",
      "}",
      "",
      "template <typename Owner, typename StoreRoot, typename PrivMap, typename Field, typename Value, typename QueueFn>",
      "inline Value storeFieldSet(",
      "    const Owner &owner,",
      "    StoreRoot &,",
      "    PrivMap &,",
      "    const char *key,",
      "    Field &field,",
      "    Value value,",
      "    QueueFn) {",
      "  storeAssignField(field, value);",
      "  if constexpr (requires { owner.__gea_hub.notify(key); }) { owner.__gea_hub.notify(key); }",
      "  return value;",
      "}",
      "",
      "template <typename Owner, typename StoreRoot, typename PrivMap, typename Array, typename Value, typename QueueFn>",
      "inline Value storeVectorItemPropertySet(",
      "    const Owner &owner,",
      "    StoreRoot &,",
      "    PrivMap &,",
      "    const char *field_key,",
      "    Array &array,",
      "    std::size_t index,",
      "    const char *property_key,",
      "    Value value,",
      "    QueueFn) {",
      "  if (index >= array.size()) array.resize(index + 1);",
      "  auto &slot = array[index];",
      "  set_typed_field(slot, property_key, value);",
      "  if constexpr (requires { owner.__gea_hub.notify(field_key); }) { owner.__gea_hub.notify(field_key); }",
      "  return value;",
      "}",
      "",
      // Boxed multi-store mount path can emit bindReactiveApply() even in this
      // (typed/hub) mode, so its definition must be present here too.
      ...bindReactiveApplyDef(),
      ...keyedEntryRecordDef(),
      ...changeRecordHelperSource(),
    ];
  }
  return [
    ...bindReactiveApplyDef(),
    "template <typename TrackRead>",
    "inline void compiledStoreTrackReadIfActive(bool active, const gea_cpp_value &store_root, const char *property_name, TrackRead track_read) {",
    "  if (active) track_read(store_root, property_name);",
    "}",
    "",
    "inline bool compiledStorePlainValue(const gea_cpp_value &value) {",
    "  const gea_cpp_value *candidate = &value;",
    "  // Protocol-owned proxy targets are immutable snapshots, so this chain cannot cycle.",
    "  while (candidate->kind == gea_cpp_value::kind_t::proxy && candidate->rare_r().proxy_target) {",
    "    candidate = candidate->rare_r().proxy_target.get();",
    "  }",
    "  if (candidate->kind == gea_cpp_value::kind_t::array_value) return candidate->rare_r().typed_array_name.empty();",
    "  if (candidate->kind != gea_cpp_value::kind_t::record) return false;",
    "  if (candidate->rare_r().prototype_names && !candidate->rare_r().prototype_names->empty()) return false;",
    "  const auto &prototype = candidate->rare_r().prototype_object;",
    "  return !prototype || prototype->kind == gea_cpp_value::kind_t::null_value ||",
    "         gea_cpp_strict_equals(*prototype, gea_cpp_object_prototype());",
    "}",
    "",
    "inline const gea_cpp_value &dirtySymbol() {",
    '  static const gea_cpp_value value = gea_cpp_value::symbol(std::string("gea.d.dirty"));',
    "  return value;",
    "}",
    "",
    ...changeRecordHelperSource(),
    ...keyedEntryRecordDef(),
    "inline bool storeValueNonEmpty(const gea_cpp_value &value) {",
    "  if (!gea::runtime::coerce::to_boolean(value)) return false;",
    "  if (value.kind == gea_cpp_value::kind_t::set_value) return value.set_storage && value.set_storage->size() > 0;",
    "  if (value.kind == gea_cpp_value::kind_t::map_value) return value.map_storage && value.map_storage->size() > 0;",
    "  return value.size() > 0;",
    "}",
    "",
    "inline bool storeHasQueuedConsumers(const gea_cpp_value &state, const char *key) {",
    '  auto observers = gea_cpp_key(state.record_get_literal("observers"));',
    "  if (gea::runtime::coerce::to_boolean(observers)) {",
    "    auto bucket = gea_cpp_key(gea_cpp_map_like_get_literal(observers, key));",
    "    if (storeValueNonEmpty(bucket)) return true;",
    "  }",
    '  if (storeValueNonEmpty(gea_cpp_key(state.record_get_literal("rootObservers")))) return true;',
    '  if (storeValueNonEmpty(gea_cpp_key(state.record_get_literal("derived")))) return true;',
    "  return false;",
    "}",
    "",
    "template <typename Field, typename Value>",
    "inline void storeAssignField(Field &field, const Value &value) {",
    "  if constexpr (std::is_same_v<std::decay_t<Field>, std::string>) {",
    "    if constexpr (std::is_same_v<std::decay_t<Value>, std::string>) {",
    "      field = value;",
    "    } else if constexpr (std::is_convertible_v<Value, std::string>) {",
    "      field = std::string(value);",
    "    } else {",
    "      field = gea_cpp_to_string(gea_cpp_value(value));",
    "    }",
    "  } else if constexpr (std::is_same_v<std::decay_t<Value>, std::vector<gea_cpp_value>> && requires(Field &out, const gea_cpp_value &item) { typename std::decay_t<Field>::value_type; out.clear(); out.reserve(std::size_t{}); out.push_back(typename std::decay_t<Field>::value_type(item)); }) {",
    "    field.clear();",
    "    field.reserve(value.size());",
    "    for (const auto &item : value) field.push_back(typename std::decay_t<Field>::value_type(item));",
    "  } else {",
    "    field = static_cast<Field>(value);",
    "  }",
    "}",
    "",
    // Per-store-method batching of keyed-list direct dispatch. A bulk update
    // (e.g. re-filling 14 forecast rows = ~42 item-field writes) would otherwise
    // re-run each affected keyed list\'s `.map` on EVERY write (~36ms each).
    // While a batch is open (begin/end wrap each store method; nested calls share
    // the outermost batch) the per-item direct handlers are recorded by
    // (state,key) and run ONCE at the outermost method exit instead.
    "inline int &storeDirectBatchDepth() { static int depth = 0; return depth; }",
    "inline std::vector<std::pair<gea_cpp_value, std::string>> &storeDirectBatchPending() {",
    "  static std::vector<std::pair<gea_cpp_value, std::string>> pending;",
    "  return pending;",
    "}",
    "inline const void *storeStateIdentity(const gea_cpp_value &state) {",
    "  return state.entries ? static_cast<const void *>(state.entries.get()) : static_cast<const void *>(nullptr);",
    "}",
    "inline void storeDirectBatchBegin() { storeDirectBatchDepth()++; }",
    "inline void storeRunDirectField(const gea_cpp_value &state, const char *key) {",
    '  auto direct_map = gea_cpp_key(state.record_get_literal("direct"));',
    "  if (!gea::runtime::coerce::to_boolean(direct_map)) return;",
    "  auto direct = gea_cpp_key(gea_cpp_map_like_get_literal(direct_map, key));",
    "  if (!gea::runtime::coerce::to_boolean(direct)) return;",
    "  for (auto &handler : direct) { (void)(handler()); }",
    "}",
    "inline void storeDirectBatchRecord(const gea_cpp_value &state, const char *key) {",
    "  const void *id = storeStateIdentity(state);",
    "  auto &pending = storeDirectBatchPending();",
    "  for (const auto &entry : pending) {",
    "    if (entry.second == key && storeStateIdentity(entry.first) == id) return;",
    "  }",
    "  pending.emplace_back(state, std::string(key));",
    "}",
    "inline void storeDirectBatchEnd() {",
    "  if (storeDirectBatchDepth() > 0) storeDirectBatchDepth()--;",
    "  if (storeDirectBatchDepth() != 0) return;",
    "  if (storeDirectBatchPending().empty()) return;",
    "  std::vector<std::pair<gea_cpp_value, std::string>> run;",
    "  run.swap(storeDirectBatchPending());",
    "  for (auto &entry : run) { storeRunDirectField(entry.first, entry.second.c_str()); }",
    "}",
    "",
    // RAII batch guard for a whole store method: begin a direct-notification',
    "// batch on entry, flush it on exit (any path — return, early return, throw).",
    "// This coalesces all reactive notifications a method makes into a single",
    "// flush at method exit, matching the batched semantics the lowered-method",
    "// path produced with storeDirectBatchBegin()/flush/End wrappers.",
    "struct StoreMethodBatch {",
    "  StoreMethodBatch() { storeDirectBatchBegin(); }",
    "  ~StoreMethodBatch() { storeDirectBatchEnd(); }",
    "  StoreMethodBatch(const StoreMethodBatch &) = delete;",
    "  StoreMethodBatch &operator=(const StoreMethodBatch &) = delete;",
    "};",
    "",
    "template <typename Owner, typename PrivMap, typename Field, typename Value, typename QueueFn>",
    "inline Value storeFieldSet(",
    "    const Owner &owner,",
    "    gea_cpp_value &self_value,",
    "    PrivMap &priv,",
    "    const char *key,",
    "    Field &field,",
    "    Value value,",
    "    QueueFn queue_fn) {",
    "  auto old_value = gea_cpp_key(field);",
    "  auto stored_value = gea_cpp_key(value);",
    "  if (!gea_cpp_same_value_zero(old_value, stored_value)) {",
    "    storeAssignField(field, value);",
    // Fallback-lowered store methods (async, try/catch, typed JSON.parse — any
    // body the re-lowerer cannot take) write through THIS helper, not the
    // re-lowered dirty-flush path. Typed-hub renderer bindings only listen on
    // the hub, so without this notify a field written exclusively from a
    // fallback method (weather\'s applyApiWeather panel fields) never
    // re-renders. Hub appliers are microtask-scheduled and deduped, so the
    // extra notify on dual-path writes is harmless.
    "    if constexpr (requires { owner.__gea_hub.notify(key); }) { owner.__gea_hub.notify(key); }",
    "    auto state = gea_cpp_key(priv.get(self_value));",
    '    auto direct_map = gea_cpp_key(state.record_get_literal("direct"));',
    "    if (gea::runtime::coerce::to_boolean(direct_map)) {",
    "      if (storeDirectBatchDepth() > 0) {",
    "        storeDirectBatchRecord(state, key);",
    "      } else {",
    "        auto direct = gea_cpp_key(gea_cpp_map_like_get_literal(direct_map, key));",
    "        if (gea::runtime::coerce::to_boolean(direct)) {",
    "          for (auto &handler : direct) {",
    "            (void)(handler(stored_value));",
    "          }",
    "        }",
    "      }",
    "    }",
    '    auto observers = gea_cpp_key(state.record_get_literal("observers"));',
    "    bool has_observers = gea::runtime::coerce::to_boolean(observers) && storeValueNonEmpty(gea_cpp_key(gea_cpp_map_like_get_literal(observers, key)));",
    "    if (has_observers) {",
    "      (void)(queue_fn(self_value, state, std::string(key), changeRecord(old_value, stored_value)));",
    "    }",
    "  }",
    "  return value;",
    "}",
    "",
    // Generic over the array's element type so typed-storage stores
    // (`std::vector<X_item>`) and dynamic stores
    // (`std::vector<gea_cpp_value>`) both link the same fallback path.
    // For typed slots the inner `if constexpr` branch writes the field
    // directly via `set_typed_field`; legacy dynamic slots keep the
    // `record_set_literal` + dirty-symbol path. The previous-value
    // snapshot still goes through `gea_cpp_key(slot)` which is overloaded
    // for the typed item struct (see `cpp-stores.ts`).
    "template <typename Owner, typename PrivMap, typename Array, typename Value, typename QueueFn>",
    "inline Value storeVectorItemPropertySet(",
    "    const Owner &owner,",
    "    gea_cpp_value &self_value,",
    "    PrivMap &priv,",
    "    const char *field_key,",
    "    Array &array,",
    "    std::size_t index,",
    "    const char *property_key,",
    "    Value value,",
    "    QueueFn queue_fn) {",
    "  using __GeaSlotElement = typename std::decay_t<Array>::value_type;",
    "  if (index >= array.size()) array.resize(index + 1);",
    "  auto &slot = array[index];",
    "  if constexpr (std::is_same_v<__GeaSlotElement, gea_cpp_value>) {",
    "    if (slot.is_nullish()) {",
    "      slot.kind = gea_cpp_value::kind_t::record;",
    "      slot.entries = std::make_shared<std::vector<std::pair<std::string, gea_cpp_value>>>();",
    "    }",
    "  }",
    "  auto state = gea_cpp_key(priv.get(self_value));",
    "  const bool has_queued_consumers = storeHasQueuedConsumers(state, field_key);",
    "  auto old_value = has_queued_consumers ? gea_cpp_snapshot_value(gea_cpp_key(slot)) : gea_cpp_value::missing();",
    "  if constexpr (std::is_same_v<__GeaSlotElement, gea_cpp_value>) {",
    "    slot.record_set_literal(property_key, gea_cpp_key(value));",
    // Direct gea_cpp_value construction, NOT gea_cpp_key(true): inside
    // namespace gea_ir the record/NodeHandle gea_cpp_key overloads hide the
    // runtime's global generic template, and none of them accepts bool.
    "    slot.record_set(dirtySymbol(), gea_cpp_value(true));",
    "  } else {",
    "    set_typed_field(slot, property_key, value);",
    "  }",
    // Same fallback-method coverage as storeFieldSet: keyed-list bindings for
    // compiled-base stores subscribe on the typed hub, so item writes from
    // fallback-lowered methods must notify it or the list never re-renders.
    "  if constexpr (requires { owner.__gea_hub.notify(field_key); }) { owner.__gea_hub.notify(field_key); }",
    '  auto direct_map = gea_cpp_key(state.record_get_literal("direct"));',
    "  if (gea::runtime::coerce::to_boolean(direct_map)) {",
    "    if (storeDirectBatchDepth() > 0) {",
    "      storeDirectBatchRecord(state, field_key);",
    "    } else {",
    "      auto direct = gea_cpp_key(gea_cpp_map_like_get_literal(direct_map, field_key));",
    "      if (gea::runtime::coerce::to_boolean(direct)) {",
    "        for (auto &handler : direct) {",
    "          (void)(handler());",
    "        }",
    "      }",
    "    }",
    "  }",
    "  if (has_queued_consumers) {",
    "    (void)(queue_fn(",
    "      self_value,",
    "      state,",
    "      std::string(field_key),",
    "      arrayItemChange(static_cast<double>(index), old_value, gea_cpp_snapshot_value(gea_cpp_key(slot)), true)));",
    "  }",
    "  return value;",
    "}",
    "",
  ];
}

function changeRecordHelperSource(): string[] {
  return [
    "inline gea_cpp_value changeRecord(gea_cpp_value previous_value, gea_cpp_value new_value) {",
    "  gea_cpp_value out = gea_cpp_value::empty_record();",
    '  out.record_set_literal("previousValue", std::move(previous_value));',
    '  out.record_set_literal("newValue", std::move(new_value));',
    "  return out;",
    "}",
    "",
    "inline gea_cpp_value listChangeRecord(const char *type, double start = 0.0, double count = 0.0, bool has_range = false) {",
    "  gea_cpp_value out = gea_cpp_value::empty_record();",
    '  out.record_set_literal("type", gea_cpp_value(std::string(type)));',
    "  if (has_range) {",
    '    out.record_set_literal("start", gea_cpp_value(start));',
    '    out.record_set_literal("count", gea_cpp_value(count));',
    "  }",
    "  return out;",
    "}",
    "",
    "inline gea_cpp_value arrayItemChange(double index, gea_cpp_value previous_value, gea_cpp_value new_value, bool item_dirty = false) {",
    "  gea_cpp_value out = gea_cpp_value::empty_record();",
    '  out.record_set_literal("aipu", gea_cpp_value(true));',
    '  out.record_set_literal("arix", gea_cpp_value(index));',
    '  out.record_set_literal("previousValue", std::move(previous_value));',
    '  out.record_set_literal("newValue", std::move(new_value));',
    '  if (item_dirty) out.record_set_literal("itemDirty", gea_cpp_value(true));',
    "  return out;",
    "}",
    "",
  ];
}

/**
 * What the consuming compiler's value model provides.
 *
 * geatsc v1 always has its boxed carrier (`gea_cpp_value`) and its
 * single-precision scalar (`gea_f32`), and the helpers below carry overloads
 * that exist only to accept them. A consumer whose scalars are plain `double`
 * and which never boxes -- geatsc -- has neither type in the translation
 * unit, so those four overloads would name an undeclared type. Passing
 * `boxedValueModel: false` omits them and forward-declares `gea_f32`, which is
 * all the surviving `std::is_same_v<..., gea_f32>` tests need.
 *
 * Everything else in the block is unconditional: the dispatch it encodes --
 * RRGGBBAA colours to native pixels, the typed-array pointer path, the
 * string-versus-colour fill -- is what the ENGINE requires of any caller, and
 * a second copy of it in another compiler would be a second authority over the
 * same question.
 */
export interface CanvasInteropOptions {
  readonly boxedValueModel?: boolean;
}

export function directCanvasInteropSource(
  usesCanvas: boolean,
  options: CanvasInteropOptions = {},
): string[] {
  if (!usesCanvas) return [];
  const boxed = options.boxedValueModel !== false;
  return [
    ...(boxed ? [] : ["struct gea_f32;", ""]),
    // The DOM helpers this namespace's own shim rows name, for the UNBOXED
    // model only. `domAppendChild` is stated once in the boxed generator
    // (`generateCppIrNamespaceSource`, alongside four `gea_cpp_value`
    // overloads), and a unit built on the unboxed model never carries that
    // block -- so `nativeMemberMethods.appendChild`, which spells
    // `gea_ir::domAppendChild({receiver}, {arg0})`, named a function nothing in
    // such a unit defined. Emitted here, guarded, rather than by changing the
    // row: the row is right, and the boxed unit must not get a second
    // definition of the same overload.
    //
    // Returns the appended child, mirroring DOM `Node.appendChild`, exactly as
    // the boxed definition does.
    ...(boxed
      ? []
      : [
          "inline gea::embedded::ui::NodeHandle domAppendChild(gea::embedded::ui::NodeHandle parent, gea::embedded::ui::NodeHandle child) {",
          "  if (child.valid()) parent.appendChild(child);",
          "  return child;",
          "}",
          "",
        ]),
    "inline gea::embedded::ui::CanvasRenderingContext2D getCanvasContext(gea::embedded::ui::NodeHandle node, const std::string &kind) {",
    '  if (kind != "2d") return gea::embedded::ui::CanvasRenderingContext2D();',
    "  return gea::embedded::ui::CanvasElement(node.id()).getContext2D();",
    "}",
    "",
    ...(boxed
      ? [
          "inline gea::embedded::ui::CanvasRenderingContext2D getCanvasContext(gea::embedded::ui::NodeHandle node, const std::string &kind, const gea_cpp_value &) {",
          "  return getCanvasContext(node, kind);",
          "}",
          "",
        ]
      : []),
    // geatsc stores a NULLABLE typed handle (`Component<T>`\'s `el: T | null`)
    // as std::optional<NodeHandle>. Without these unwrapping overloads the
    // optional binds the generic gea_cpp_value parameter instead, boxing an
    // OPAQUE object whose record carries no __gea_node_id — the context then
    // binds node -1 and every draw silently no-ops (maps: black screen).
    "inline gea::embedded::ui::CanvasRenderingContext2D getCanvasContext(const std::optional<gea::embedded::ui::NodeHandle> &node, const std::string &kind) {",
    "  return getCanvasContext(node.value_or(gea::embedded::ui::NodeHandle()), kind);",
    "}",
    "",
    ...(boxed
      ? [
          "inline gea::embedded::ui::CanvasRenderingContext2D getCanvasContext(const std::optional<gea::embedded::ui::NodeHandle> &node, const std::string &kind, const gea_cpp_value &) {",
          "  return getCanvasContext(node.value_or(gea::embedded::ui::NodeHandle()), kind);",
          "}",
          "",
        ]
      : []),
    "inline float canvasNumber(float value) { return value; }",
    "inline float canvasNumber(double value) { return static_cast<float>(value); }",
    "inline float canvasNumber(int8_t value) { return static_cast<float>(value); }",
    "inline float canvasNumber(uint8_t value) { return static_cast<float>(value); }",
    "inline float canvasNumber(int16_t value) { return static_cast<float>(value); }",
    "inline float canvasNumber(uint16_t value) { return static_cast<float>(value); }",
    "inline float canvasNumber(int value) { return static_cast<float>(value); }",
    "inline float canvasNumber(uint32_t value) { return static_cast<float>(value); }",
    "inline float canvasNumber(long long value) { return static_cast<float>(value); }",
    ...(boxed ? ['inline float canvasNumber(const gea_f32 &value) { return value.v; }'] : []),
    "template <typename T>",
    "inline float canvasNumber(const T &value) {",
    "  if constexpr (std::is_same_v<std::decay_t<T>, gea_f32>) return value.v;",
    "  else if constexpr (std::is_arithmetic_v<std::decay_t<T>>) return static_cast<float>(value);",
    // Not a discarded branch to a consumer without geatsc's runtime: the
    // names here are NON-dependent, so a compiler resolves them where the
    // template is written rather than where it is instantiated. The
    // static_assert IS dependent, so it fires only if the branch is ever
    // selected -- and then it names the unsupported argument type instead of
    // reporting a missing namespace at the definition.
    ...(boxed
      ? [
          "  else return gea::runtime::numeric::to_float(gea::runtime::coerce::to_number(gea_cpp_key(value)));",
        ]
      : [
          "  else {",
          '    static_assert(sizeof(T) == 0, "canvasNumber: no numeric conversion for this argument type");',
          "    return 0.0f;",
          "  }",
        ]),
    "}",
    "",
    "inline int canvasRound(float value) {",
    "  if (!std::isfinite(value)) return 0;",
    "  return static_cast<int>(value + (value >= 0.0f ? 0.5f : -0.5f));",
    "}",
    "inline int canvasInt(int8_t value) { return static_cast<int>(value); }",
    "inline int canvasInt(uint8_t value) { return static_cast<int>(value); }",
    "inline int canvasInt(int16_t value) { return static_cast<int>(value); }",
    "inline int canvasInt(uint16_t value) { return static_cast<int>(value); }",
    "inline int canvasInt(int value) { return value; }",
    "inline int canvasInt(uint32_t value) { return static_cast<int>(value); }",
    "inline int canvasInt(long long value) { return static_cast<int>(value); }",
    "inline int canvasInt(double value) { return canvasRound(static_cast<float>(value)); }",
    "inline int canvasInt(float value) { return canvasRound(value); }",
    ...(boxed ? ['inline int canvasInt(const gea_f32 &value) { return canvasRound(value.v); }'] : []),
    "template <typename T>",
    "inline int canvasInt(const T &value) { return canvasRound(canvasNumber(value)); }",
    "",
    "inline std::string canvasString(const std::string &value) { return value; }",
    "inline std::string canvasString(const char *value) { return value ? std::string(value) : std::string(); }",
    "template <typename T>",
    "inline std::string canvasString(const T &value) {",
    "  if constexpr (std::is_convertible_v<T, std::string>) return std::string(value);",
    "  else if constexpr (std::is_arithmetic_v<std::decay_t<T>>) return std::to_string(value);",
    "  else if constexpr (std::is_same_v<std::decay_t<T>, gea_f32>) return std::to_string(value.v);",
    "  else return std::string();",
    "}",
    "",
    "inline gea::framework::graphics::pixel::native_t canvasRgb565(gea::framework::graphics::pixel::NativeColor value) { return value.value; }",
    "template <typename T>",
    "inline gea::framework::graphics::pixel::native_t canvasRgb565(const T &value) { return gea::framework::graphics::pixel::nativeFromRrggbbaa(static_cast<std::uint32_t>(canvasNumber(value))); }",
    "template <typename T>",
    "inline constexpr bool canvasColorValue = std::is_arithmetic_v<std::decay_t<T>> || std::is_same_v<std::decay_t<T>, gea::framework::graphics::pixel::NativeColor> || std::is_same_v<std::decay_t<T>, gea_f32>;",
    "",
    "template <typename T>",
    "inline int canvasImageId(const T &value) {",
    "  if constexpr (requires { value.id; }) return canvasInt(value.id);",
    "  else if constexpr (std::is_arithmetic_v<std::decay_t<T>> || std::is_same_v<std::decay_t<T>, gea_f32>) return canvasInt(value);",
    "  else return -1;",
    "}",
    "",
    "inline void canvasClear(gea::embedded::ui::CanvasRenderingContext2D &ctx) {",
    "  ctx.clear();",
    "}",
    "",
    "template <typename X, typename Y, typename W, typename H>",
    "inline void canvasClearRect(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X &x, const Y &y, const W &w, const H &h) {",
    "  ctx.clearRect(canvasInt(x), canvasInt(y), canvasInt(w), canvasInt(h));",
    "}",
    "",
    "template <typename X, typename Y, typename W, typename H>",
    "inline void canvasFillRect(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X &x, const Y &y, const W &w, const H &h) {",
    "  ctx.fillRect(canvasInt(x), canvasInt(y), canvasInt(w), canvasInt(h));",
    "}",
    "",
    "template <typename X, typename Y, typename W, typename H>",
    "inline void canvasStrokeRect(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X &x, const Y &y, const W &w, const H &h) {",
    "  ctx.strokeRect(canvasInt(x), canvasInt(y), canvasInt(w), canvasInt(h));",
    "}",
    "",
    "template <typename X, typename Y, typename R, typename Fill>",
    "inline void canvasFillCircle(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X &x, const Y &y, const R &r, const Fill &fill) {",
    "  if constexpr (canvasColorValue<Fill>) {",
    "    ctx.fillCircleRgb565(canvasInt(x), canvasInt(y), canvasInt(r), canvasRgb565(fill));",
    "  } else {",
    "    ctx.setFillStyle(canvasString(fill));",
    "    ctx.fillCircle(canvasInt(x), canvasInt(y), canvasInt(r));",
    "  }",
    "}",
    "",
    "template <typename X, typename Y, typename R>",
    "inline void canvasStrokeCircle(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X &x, const Y &y, const R &r) {",
    "  ctx.strokeCircle(canvasInt(x), canvasInt(y), canvasInt(r));",
    "}",
    "",
    "template <typename X, typename Y, typename R, typename Fill>",
    "inline void canvasFillCircleRgb565(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X &x, const Y &y, const R &r, const Fill &fill) {",
    "  ctx.fillCircleRgb565(canvasInt(x), canvasInt(y), canvasInt(r), canvasRgb565(fill));",
    "}",
    "",
    "template <typename X0, typename Y0, typename X1, typename Y1, typename X2, typename Y2, typename Fill>",
    "inline void canvasFillTriangleRgb565(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X0 &x0, const Y0 &y0, const X1 &x1, const Y1 &y1, const X2 &x2, const Y2 &y2, const Fill &fill) {",
    "  ctx.fillTriangleRgb565(canvasInt(x0), canvasInt(y0), canvasInt(x1), canvasInt(y1), canvasInt(x2), canvasInt(y2), canvasRgb565(fill));",
    "}",
    "",
    "template <typename XS, typename YS, typename R, typename Colors>",
    "inline void canvasFillCirclesRgb565(gea::embedded::ui::CanvasRenderingContext2D &ctx, const XS &xs, const YS &ys, const R &r, const Colors &colors, int count = -1) {",
    "  using __gea_color_t = std::decay_t<typename Colors::value_type>;",
    "  if constexpr (std::is_same_v<__gea_color_t, gea::framework::graphics::pixel::native_t> || std::is_same_v<__gea_color_t, gea::framework::graphics::pixel::NativeColor>) {",
    "    if constexpr (requires { xs.data(); ys.data(); colors.data(); } && std::is_same_v<std::decay_t<decltype(*xs.data())>, std::uint16_t> && std::is_same_v<std::decay_t<decltype(*ys.data())>, std::uint16_t>) {",
    "      // Pointer path. Passing the containers themselves binds a typed array",
    "      // through its implicit operator std::vector<Element>(), which heap-",
    "      // allocates and copies the FULL array on every call while ignoring",
    "      // `count` -- ~150us per call on esp32-s3 regardless of how many",
    "      // circles it carried.",
    "      std::size_t __gea_n = xs.size() < ys.size() ? xs.size() : ys.size();",
    "      if (colors.size() < __gea_n) __gea_n = colors.size();",
    "      if (count >= 0 && static_cast<std::size_t>(count) < __gea_n) __gea_n = static_cast<std::size_t>(count);",
    "      ctx.fillCirclesRgb565(xs.data(), ys.data(), canvasInt(r), colors.data(), static_cast<int>(__gea_n));",
    "    } else {",
    "      ctx.fillCirclesRgb565(xs, ys, canvasInt(r), colors, count);",
    "    }",
    "  } else {",
    "    std::size_t __gea_n = colors.size();",
    "    if (count >= 0 && static_cast<std::size_t>(count) < __gea_n) __gea_n = static_cast<std::size_t>(count);",
    "    std::vector<gea::framework::graphics::pixel::native_t> __gea_native_colors;",
    "    __gea_native_colors.reserve(__gea_n);",
    "    for (std::size_t __gea_i = 0; __gea_i < __gea_n; ++__gea_i) __gea_native_colors.push_back(canvasRgb565(colors[__gea_i]));",
    "    ctx.fillCirclesRgb565(xs, ys, canvasInt(r), __gea_native_colors, count);",
    "  }",
    "}",
    "",
    "template <typename XS, typename YS, typename R, typename Fill>",
    "inline void canvasFillCirclesRgb565Uniform(gea::embedded::ui::CanvasRenderingContext2D &ctx, const XS &xs, const YS &ys, const R &r, const Fill &fill) {",
    "  ctx.fillCirclesRgb565(xs, ys, canvasInt(r), canvasRgb565(fill));",
    "}",
    "",
    "template <typename X0s, typename Y0s, typename X1s, typename Y1s, typename X2s, typename Y2s, typename Colors, typename Order>",
    "inline void canvasFillTrianglesRgb565Sorted(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X0s &x0s, const Y0s &y0s, const X1s &x1s, const Y1s &y1s, const X2s &x2s, const Y2s &y2s, const Colors &colors, const Order &order, int count = -1) {",
    "  // Colours are authored 0xRRGGBBAA (plain numbers, e.g. a Uint32Array);",
    "  // the canvas layer converts during packing. `order` gives the paint",
    "  // (depth) order into the coordinate/colour arrays; `count` (>=0) limits",
    "  // to the first N entries so callers can reuse fixed-size scratch arrays.",
    "  ctx.fillTrianglesRgb565Sorted(x0s, y0s, x1s, y1s, x2s, y2s, colors, order, count);",
    "}",
    "",
    "inline void canvasBeginPath(gea::embedded::ui::CanvasRenderingContext2D &ctx) {",
    "  ctx.beginPath();",
    "}",
    "",
    "template <typename X, typename Y, typename R, typename Start, typename End>",
    "inline void canvasArc(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X &x, const Y &y, const R &r, const Start &start, const End &end) {",
    "  ctx.arc(canvasNumber(x), canvasNumber(y), canvasNumber(r), canvasNumber(start), canvasNumber(end));",
    "}",
    "",
    "template <typename X, typename Y>",
    "inline void canvasMoveTo(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X &x, const Y &y) {",
    "  ctx.moveTo(canvasNumber(x), canvasNumber(y));",
    "}",
    "",
    "template <typename X, typename Y>",
    "inline void canvasLineTo(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X &x, const Y &y) {",
    "  ctx.lineTo(canvasNumber(x), canvasNumber(y));",
    "}",
    "",
    "inline void canvasClosePath(gea::embedded::ui::CanvasRenderingContext2D &ctx) {",
    "  ctx.closePath();",
    "}",
    "",
    "inline void canvasFill(gea::embedded::ui::CanvasRenderingContext2D &ctx) {",
    "  ctx.fill();",
    "}",
    "",
    "inline void canvasStroke(gea::embedded::ui::CanvasRenderingContext2D &ctx) {",
    "  ctx.stroke();",
    "}",
    "",
    "template <typename Text, typename X, typename Y>",
    "inline void canvasFillText(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Text &text, const X &x, const Y &y) {",
    "  ctx.fillText(canvasString(text), canvasInt(x), canvasInt(y));",
    "}",
    "",
    "template <typename Image, typename X, typename Y>",
    "inline void canvasDrawImage(gea::embedded::ui::CanvasRenderingContext2D ctx, const Image &image, const X &x, const Y &y) {",
    "  ctx.drawImage(canvasImageId(image), canvasInt(x), canvasInt(y));",
    "}",
    "",
    "template <typename Image, typename X, typename Y, typename W, typename H>",
    "inline void canvasDrawImage(gea::embedded::ui::CanvasRenderingContext2D ctx, const Image &image, const X &x, const Y &y, const W &w, const H &h) {",
    "  ctx.drawImage(canvasImageId(image), canvasInt(x), canvasInt(y), canvasInt(w), canvasInt(h));",
    "}",
    "",
    "template <typename Text>",
    "inline double canvasMeasureTextInkCenter(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Text &text) {",
    "  return ctx.measureTextInkCenter(canvasString(text));",
    "}",
    "",
    "template <typename Text>",
    "inline double canvasMeasureText(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Text &text) {",
    "  return ctx.measureText(canvasString(text));",
    "}",
    "",
    "template <typename Image, typename X, typename Y, typename W, typename H>",
    "inline void canvasDrawImageCircle(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Image &image, const X &x, const Y &y, const W &w, const H &h) {",
    "  ctx.drawImageCircle(canvasImageId(image), canvasInt(x), canvasInt(y), canvasInt(w), canvasInt(h));",
    "}",
    "",
    "template <typename Image, typename X, typename Y, typename W, typename H>",
    "inline void canvasDrawImageRotated90CW(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Image &image, const X &x, const Y &y, const W &w, const H &h) {",
    "  ctx.drawImageRotated90CW(canvasImageId(image), canvasInt(x), canvasInt(y), canvasInt(w), canvasInt(h));",
    "}",
    "",
    "template <typename Image, typename X, typename Y, typename W>",
    "inline void canvasDrawImageTiledX(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Image &image, const X &x, const Y &y, const W &w) {",
    "  ctx.drawImageTiledX(canvasImageId(image), canvasInt(x), canvasInt(y), canvasInt(w));",
    "}",
    "",
    "inline void canvasFlush(gea::embedded::ui::CanvasRenderingContext2D &ctx) {",
    "  ctx.flush();",
    "}",
    "",
    "inline void canvasBeginBatch(gea::embedded::ui::CanvasRenderingContext2D &ctx) {",
    "  ctx.beginBatch();",
    "}",
    "",
    "inline void canvasEndBatch(gea::embedded::ui::CanvasRenderingContext2D &ctx) {",
    "  ctx.endBatch();",
    "}",
    "",
    "template <typename Value>",
    "inline const Value &canvasSetFillStyle(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Value &value) {",
    "  if constexpr (canvasColorValue<Value>) ctx.setFillStyleRgb565(canvasRgb565(value));",
    "  else ctx.setFillStyle(canvasString(value));",
    "  return value;",
    "}",
    "",
    "inline gea::framework::graphics::pixel::native_t canvasSetFillStyleRgb565(gea::embedded::ui::CanvasRenderingContext2D &ctx, gea::framework::graphics::pixel::native_t value) {",
    "  ctx.setFillStyleRgb565(value);",
    "  return value;",
    "}",
    "",
    "template <typename Value>",
    "inline const Value &canvasSetStrokeStyle(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Value &value) {",
    "  if constexpr (canvasColorValue<Value>) ctx.setStrokeStyleRgb565(canvasRgb565(value));",
    "  else ctx.setStrokeStyle(canvasString(value));",
    "  return value;",
    "}",
    "",
    "template <typename Value>",
    "inline const Value &canvasSetGlobalAlpha(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Value &value) {",
    "  ctx.setGlobalAlpha(canvasNumber(value));",
    "  return value;",
    "}",
    "",
    "template <typename Value>",
    "inline const Value &canvasSetLineWidth(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Value &value) {",
    "  ctx.setLineWidth(canvasNumber(value));",
    "  return value;",
    "}",
    "",
    "template <typename Value>",
    "inline const Value &canvasSetFont(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Value &value) {",
    "  ctx.setFont(canvasString(value));",
    "  return value;",
    "}",
    "",
    "template <typename Value>",
    "inline const Value &canvasSetTextBaseline(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Value &value) {",
    "  ctx.setTextBaseline(canvasString(value));",
    "  return value;",
    "}",
    "",
    "template <typename Value>",
    "inline const Value &canvasSetTextAlign(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Value &value) {",
    "  ctx.setTextAlign(canvasString(value));",
    "  return value;",
    "}",
    "",
  ];
}

function canvasInteropSource(usesCanvas: boolean, directCanvas: boolean): string[] {
  if (directCanvas)
    return [
      ...domStyleInteropSource(),
      ...directCanvasInteropSource(usesCanvas),
    ];
  return [
    ...domStyleInteropSource(),
    "",
    "inline int nodeIdFromValue(const gea_cpp_value &value) {",
    '  gea_cpp_value id = value.record_get_literal("__gea_node_id");',
    "  if (id.is_nullish()) return -1;",
    "  const double raw = gea::runtime::coerce::to_number(id);",
    "  if (!(raw == raw)) return -1;",
    "  return static_cast<int>(raw);",
    "}",
    "",
    "// Native event-listener registration for imperative `el.addEventListener(...)`.",
    "// The generated handler body is already polymorphic over the event arg, so the",
    "// emitter types the listener param as `PointerEvent &` and routes here — the",
    "// node receives the native event directly, never marshalled through a",
    "// gea_cpp_value record (which would re-box the whole typed PointerEvent on",
    "// every dispatch).",
    "inline void addNativeEventListener(gea::embedded::ui::NodeHandle node, const std::string &type, std::function<void(gea::framework::events::PointerEvent &)> listener) {",
    "  node.addEventListener(type.c_str(), std::move(listener));",
    "}",
    "",
    "inline void addNativeEventListener(const gea_cpp_value &node, const std::string &type, std::function<void(gea::framework::events::PointerEvent &)> listener) {",
    "  const int id = nodeIdFromValue(node);",
    "  if (id >= 0) gea::embedded::ui::NodeHandle(id).addEventListener(type.c_str(), std::move(listener));",
    "}",
    "",
    "inline std::string nodeStringArg(const gea_cpp_value &value) {",
    "  return value.is_nullish() ? std::string() : gea_cpp_to_string(value);",
    "}",
    "",
    "inline gea_cpp_value nodeClassListValue(gea::embedded::ui::NodeHandle node) {",
    "  gea_cpp_value value = gea_cpp_value::empty_record();",
    '  value.record_set_literal("add", gea_cpp_callable_args([node](const std::vector<gea_cpp_value> &tokens) -> gea_cpp_value {',
    "    for (const auto &token : tokens) node.classList().add(nodeStringArg(token));",
    "    return gea_cpp_value::missing();",
    "  }));",
    '  value.record_set_literal("remove", gea_cpp_callable_args([node](const std::vector<gea_cpp_value> &tokens) -> gea_cpp_value {',
    "    for (const auto &token : tokens) node.classList().remove(nodeStringArg(token));",
    "    return gea_cpp_value::missing();",
    "  }));",
    '  value.record_set_literal("contains", gea_cpp_value([node](gea_cpp_value token) -> gea_cpp_value {',
    "    return gea_cpp_value(node.classList().contains(nodeStringArg(token)));",
    "  }));",
    '  value.record_set_literal("toggle", gea_cpp_callable_args([node](const std::vector<gea_cpp_value> &args) -> gea_cpp_value {',
    "    if (args.empty()) return gea_cpp_value(false);",
    "    const std::string token = nodeStringArg(args[0]);",
    "    if (args.size() > 1 && !args[1].is_nullish()) return gea_cpp_value(node.classList().toggle(token, gea::runtime::coerce::to_boolean(args[1])));",
    "    return gea_cpp_value(node.classList().toggle(token));",
    "  }));",
    '  value.record_set_literal("set", gea_cpp_value([node](gea_cpp_value class_name) -> gea_cpp_value {',
    "    node.classList().set(nodeStringArg(class_name));",
    "    return gea_cpp_value::missing();",
    "  }));",
    '  value.record_set_literal("value", gea_cpp_value(node.classList().value()));',
    "  return value;",
    "}",
    "",
    "inline void nodeApplyStyleProperty(gea::embedded::ui::NodeHandle node, const std::string &property, const gea_cpp_value &value) {",
    "  if (value.kind == gea_cpp_value::kind_t::number) {",
    "    (void)gea::embedded::ui::StyleSheet::instance().applyNumberProperty(node, property.c_str(), value.number);",
    "    return;",
    "  }",
    "  gea::embedded::ui::StyleSheet::instance().applyProperty(node, property, nodeStringArg(value));",
    "}",
    "",
    "inline gea_cpp_value nodeStyleValue(gea::embedded::ui::NodeHandle node) {",
    "  return gea_cpp_value::object(",
    "      (static_cast<std::uintptr_t>(node.id()) << 8u) | 0x53u,",
    "      [node](const std::string &key) -> gea_cpp_value {",
    '        if (key == "setProperty") return gea_cpp_value([node](gea_cpp_value name_value, gea_cpp_value property_value) -> gea_cpp_value {',
    "          nodeApplyStyleProperty(node, nodeStringArg(name_value), property_value);",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "removeProperty") return gea_cpp_value([node](gea_cpp_value name_value) -> gea_cpp_value {',
    "          (void)gea::embedded::ui::StyleSheet::instance().removeProperty(node, nodeStringArg(name_value));",
    "          return gea_cpp_value::missing();",
    "        });",
    "        return gea_cpp_value::missing();",
    "      },",
    "      [node](const std::string &key, gea_cpp_value assigned_value) {",
    "        nodeApplyStyleProperty(node, key, assigned_value);",
    "      },",
    "      [](const std::string &key) -> bool {",
    '        return key == "setProperty" || key == "removeProperty";',
    "      });",
    "}",
    "",
    "inline gea_cpp_value pointerEventValue(gea::framework::events::PointerEvent &event);",
    "",
    ...(usesCanvas
      ? [
          "// Forward declaration: nodeValue's boxed getContext dispatch needs it; the",
          "// definition follows later in the prelude.",
          "inline gea::embedded::ui::CanvasRenderingContext2D getCanvasContext(const gea_cpp_value &node, const std::string &kind);",
          "inline gea::embedded::ui::CanvasRenderingContext2D getCanvasContext(const gea_cpp_value &node, const std::string &kind, const gea_cpp_value &attributes);",
          "",
        ]
      : []),
    "inline gea_cpp_value nodeValue(gea::embedded::ui::NodeHandle node) {",
    "  return gea_cpp_value::object(",
    "      static_cast<std::uintptr_t>(node.id() + 1),",
    "      [node](const std::string &key) -> gea_cpp_value {",
    '        if (key == "__gea_node_id") return gea_cpp_value(static_cast<double>(node.id()));',
    '        if (key == "nodeType") return gea_cpp_value(1.0);',
    '        if (key == "classList") return nodeClassListValue(node);',
    '        if (key == "className") return gea_cpp_value(node.classList().value());',
    '        if (key == "style") return nodeStyleValue(node);',
    '        if (key == "setAttribute") return gea_cpp_value([node](gea_cpp_value name_value, gea_cpp_value attr_value) -> gea_cpp_value {',
    "          const std::string name = nodeStringArg(name_value);",
    "          const std::string value = nodeStringArg(attr_value);",
    "          node.setAttribute(name.c_str(), value.c_str());",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "getAttribute") return gea_cpp_value([node](gea_cpp_value name_value) -> gea_cpp_value {',
    "          const std::string name = nodeStringArg(name_value);",
    "          return gea_cpp_value(std::string(node.getAttribute(name.c_str())));",
    "        });",
    '        if (key == "hasAttribute") return gea_cpp_value([node](gea_cpp_value name_value) -> gea_cpp_value {',
    "          const std::string name = nodeStringArg(name_value);",
    "          return gea_cpp_value(node.hasAttribute(name.c_str()));",
    "        });",
    '        if (key == "addEventListener") return gea_cpp_value([node](gea_cpp_value type_value, gea_cpp_value listener_value) -> gea_cpp_value {',
    "          const std::string type = nodeStringArg(type_value);",
    "          auto listener = gea_cpp_key(listener_value);",
    "          if (!gea::runtime::coerce::to_boolean(listener)) return gea_cpp_value::missing();",
    "          node.addEventListener(type.c_str(), [listener](gea::framework::events::PointerEvent &event) mutable {",
    "            (void)listener(pointerEventValue(event));",
    "          });",
    "          return gea_cpp_value::missing();",
    "        });",
    ...(usesCanvas
      ? [
          '        if (key == "getContext") return gea_cpp_value([node](gea_cpp_value kind_value) -> gea_cpp_value {',
          "          return geaCanvasContextValue(getCanvasContext(nodeValue(node), nodeStringArg(kind_value)));",
          "        });",
        ]
      : []),
    '        if (key == "play") return gea_cpp_value([node]() -> gea_cpp_value {',
    "          return gea_cpp_value(node.play());",
    "        });",
    '        if (key == "pause") return gea_cpp_value([node]() -> gea_cpp_value {',
    "          node.pause();",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "firstChild") {',
    "          auto child = node.firstChildHandle();",
    "          return child.valid() ? nodeValue(child) : gea_cpp_value::null_v();",
    "        }",
    '        if (key == "childNodes") {',
    "          std::vector<gea_cpp_value> kids;",
    "          for (int i = 0;; i++) {",
    "            auto c = node.childAt(i);",
    "            if (!c.valid()) break;",
    "            kids.push_back(nodeValue(c));",
    "          }",
    "          return gea_cpp_value(kids);",
    "        }",
    '        if (key == "children") {',
    "          std::vector<gea_cpp_value> kids;",
    "          const auto &__gea_tree = gea::embedded::ui::Tree::instance();",
    "          for (int i = 0;; i++) {",
    "            auto c = node.childAt(i);",
    "            if (!c.valid()) break;",
    "            const auto &__gea_child_node = __gea_tree.node(c.id());",
    "            const std::string __gea_child_tag = gea::embedded::ui::tagFromId(__gea_child_node.tag_id);",
    '            if (__gea_child_node.type == gea::embedded::ui::NodeType::Text || __gea_child_tag == "#comment" || __gea_child_tag == "#fragment" || __gea_child_tag == "#document-frag" || __gea_child_tag == "#document-fragment") continue;',
    "            kids.push_back(nodeValue(c));",
    "          }",
    "          return gea_cpp_value(kids);",
    "        }",
    '        if (key == "cloneNode") return gea_cpp_value([node](gea_cpp_value deep_value) -> gea_cpp_value {',
    "          const bool deep = deep_value.is_nullish() ? true : gea::runtime::coerce::to_boolean(gea_cpp_key(deep_value));",
    "          return nodeValue(node.cloneNode(deep));",
    "        });",
    '        if (key == "appendChild") return gea_cpp_value([node](gea_cpp_value child_value) -> gea_cpp_value {',
    "          const int child_id = nodeIdFromValue(child_value);",
    "          if (child_id >= 0) node.appendChild(gea::embedded::ui::NodeHandle(child_id));",
    "          return gea_cpp_value::missing();",
    "        });",
    '        if (key == "remove") return gea_cpp_value([node]() -> gea_cpp_value {',
    "          node.remove();",
    "          return gea_cpp_value::missing();",
    "        });",
    "        if (node.hasAttribute(key.c_str())) return gea_cpp_value(std::string(node.getAttribute(key.c_str())));",
    "        return gea_cpp_value::missing();",
    "      },",
    "      [node](const std::string &key, gea_cpp_value assigned_value) {",
    "        const std::string value = nodeStringArg(assigned_value);",
    '        if (key == "className") {',
    "          node.classList().set(value);",
    "          return;",
    "        }",
    '        if (key == "textContent" || key == "innerText") {',
    "          node.setText(value.c_str());",
    "          return;",
    "        }",
    "        node.setAttribute(key.c_str(), value.c_str());",
    "      },",
    "      [node](const std::string &key) -> bool {",
    '        return key == "__gea_node_id" || key == "nodeType" || key == "classList" || key == "className" || key == "style" ||',
    '               key == "setAttribute" || key == "getAttribute" || key == "hasAttribute" || key == "addEventListener" ||',
    '               key == "appendChild" || key == "remove" || key == "cloneNode" || key == "play" || key == "pause" || key == "firstChild" ||',
    '               key == "childNodes" || key == "children" || key == "getContext" || node.hasAttribute(key.c_str());',
    "      });",
    "}",
    "",
    "// DOM Node accessor properties on a concrete NodeHandle receiver. geatsc lowers",
    "// `node.nodeType` / `node.parentNode` to these (registered as the gea host's",
    "// nativeMemberPropertyGetters) instead of a struct member, since NodeHandle is a",
    "// lightweight tree id with no such fields. Mirrors the gea_cpp_value record",
    "// protocol (nodeValue's record_get) so both the typed-handle and value-bridged",
    "// node representations resolve these properties identically.",
    "inline double domNodeType(gea::embedded::ui::NodeHandle node) {",
    "  if (!node) return 0.0;",
    "  const auto &__gea_tree = gea::embedded::ui::Tree::instance();",
    "  const auto &__gea_n = __gea_tree.node(node.id());",
    '  if (std::string(gea::embedded::ui::tagFromId(__gea_n.tag_id)) == "#comment") return 8.0;',
    "  if (__gea_n.type == gea::embedded::ui::NodeType::Text) return 3.0;",
    "  const std::string __gea_tag = gea::embedded::ui::tagFromId(__gea_n.tag_id);",
    '  if (__gea_tag == "#fragment" || __gea_tag == "#document-frag" || __gea_tag == "#document-fragment") return 11.0;',
    "  return 1.0;",
    "}",
    "",
    "inline gea_cpp_value domParentNode(gea::embedded::ui::NodeHandle node) {",
    "  if (!node) return gea_cpp_value::null_v();",
    "  const auto &__gea_tree = gea::embedded::ui::Tree::instance();",
    "  if (node.id() < 0 || node.id() >= __gea_tree.nodeCount()) return gea_cpp_value::null_v();",
    "  const int __gea_parent = __gea_tree.node(node.id()).parent;",
    "  gea::embedded::ui::NodeHandle parent = __gea_parent >= 0 ? gea::embedded::ui::NodeHandle(__gea_parent) : gea::embedded::ui::NodeHandle();",
    "  return parent.valid() ? nodeValue(parent) : gea_cpp_value::null_v();",
    "}",
    "",
    "// Typed DOM property setters used by generated JSX template factories.",
    "inline void domSetInnerHtml(gea::embedded::ui::NodeHandle node, const std::string &value) {",
    "  gea::runtime::host::domSetInnerHtml(node, value);",
    "}",
    "",
    "inline void domSetTextContent(gea::embedded::ui::NodeHandle node, const std::string &value) {",
    "  if (node) node.setText(value.c_str());",
    "}",
    "",
    "// Same accessors for a value-bridged node. The binding matches DOM interface",
    "// names too, so a node typed `Element`/`Node` but stored as gea_cpp_value (a",
    "// value-bridged handle) routes here; recover the tree id and forward.",
    "inline double domNodeType(const gea_cpp_value &node) {",
    "  return domNodeType(gea::embedded::ui::NodeHandle(nodeIdFromValue(node)));",
    "}",
    "",
    "// DOM `Node.appendChild` RETURNS the appended child. Mirror that so the call",
    "// can be used as a value (e.g. a JSX child-append boxed in a conditional —",
    "// `cond ? gea_cpp_key(domAppendChild(...)) : …`); returning void there would",
    "// box an incomplete `void` type. In statement context the returned handle is",
    "// simply ignored.",
    "inline gea_cpp_value domAppendChild(gea::embedded::ui::NodeHandle parent, const gea_cpp_value &child) {",
    "  const int child_id = nodeIdFromValue(child);",
    "  if (child_id >= 0) parent.appendChild(gea::embedded::ui::NodeHandle(child_id));",
    "  return child;",
    "}",
    "",
    "inline gea::embedded::ui::NodeHandle domAppendChild(gea::embedded::ui::NodeHandle parent, gea::embedded::ui::NodeHandle child) {",
    "  if (child.valid()) parent.appendChild(child);",
    "  return child;",
    "}",
    "",
    "// Value-bridged parent (a `Node`/`DocumentFragment` stored as gea_cpp_value —",
    "// e.g. a keyed-list fragment): recover the tree id and forward, mirroring the",
    "// `domNodeType(const gea_cpp_value&)` delegation. Without these overloads a",
    "// `domAppendChild(frag, child)` where `frag` is a gea_cpp_value finds no viable",
    "// candidate (neither NodeHandle-parent overload accepts a gea_cpp_value).",
    "inline gea_cpp_value domAppendChild(const gea_cpp_value &parent, const gea_cpp_value &child) {",
    "  return domAppendChild(gea::embedded::ui::NodeHandle(nodeIdFromValue(parent)), child);",
    "}",
    "",
    "inline gea_cpp_value domAppendChild(const gea_cpp_value &parent, gea::embedded::ui::NodeHandle child) {",
    "  domAppendChild(gea::embedded::ui::NodeHandle(nodeIdFromValue(parent)), child);",
    "  return nodeValue(child);",
    "}",
    "",
    "inline gea_cpp_value domParentNode(const gea_cpp_value &node) {",
    "  return domParentNode(gea::embedded::ui::NodeHandle(nodeIdFromValue(node)));",
    "}",
    "",
    "// `node.ownerDocument`: one embedded document, boxed so a capability probe",
    "// (record_get('createDocumentFragment')) on the result resolves.",
    "inline gea_cpp_value domOwnerDocument(gea::embedded::ui::NodeHandle) {",
    "  return gea_cpp_key(gea::runtime::host::document());",
    "}",
    "",
    "inline gea_cpp_value domOwnerDocument(const gea_cpp_value &) {",
    "  return gea_cpp_key(gea::runtime::host::document());",
    "}",
    "",
    "inline gea_cpp_value domFirstChild(gea::embedded::ui::NodeHandle node) {",
    "  gea::embedded::ui::NodeHandle child = node.firstChildHandle();",
    "  return child.valid() ? nodeValue(child) : gea_cpp_value::null_v();",
    "}",
    "",
    "inline gea_cpp_value domFirstChild(const gea_cpp_value &node) {",
    "  return domFirstChild(gea::embedded::ui::NodeHandle(nodeIdFromValue(node)));",
    "}",
    "",
    "// `node.nextSibling`: typed NodeHandle (not boxed) so the compiled-component",
    "// keyed-list reconciler's `NodeHandle ref = entry.element.nextSibling` copy-init",
    "// and the following `container.insertBefore(ref, ...)` both stay native. An",
    "// invalid handle (no next sibling) is the DOM `null` reference insertBefore wants.",
    "inline gea::embedded::ui::NodeHandle domNextSibling(gea::embedded::ui::NodeHandle node) {",
    "  return node.nextSiblingHandle();",
    "}",
    "",
    "inline gea::embedded::ui::NodeHandle domNextSibling(const gea_cpp_value &node) {",
    "  return domNextSibling(gea::embedded::ui::NodeHandle(nodeIdFromValue(node)));",
    "}",
    "",
    ...domStyleAccessorInteropSource(),
    "inline gea_cpp_value domChildNodes(gea::embedded::ui::NodeHandle node) {",
    "  std::vector<gea_cpp_value> kids;",
    "  for (int i = 0;; i++) {",
    "    auto c = node.childAt(i);",
    "    if (!c.valid()) break;",
    "    kids.push_back(nodeValue(c));",
    "  }",
    "  return gea_cpp_value(kids);",
    "}",
    "",
    "inline gea_cpp_value domChildNodes(const gea_cpp_value &node) {",
    "  return domChildNodes(gea::embedded::ui::NodeHandle(nodeIdFromValue(node)));",
    "}",
    "",
    "inline std::vector<gea::embedded::ui::NodeHandle> domChildren(gea::embedded::ui::NodeHandle node) {",
    "  std::vector<gea::embedded::ui::NodeHandle> kids;",
    "  const auto &__gea_tree = gea::embedded::ui::Tree::instance();",
    "  for (int i = 0;; i++) {",
    "    auto c = node.childAt(i);",
    "    if (!c.valid()) break;",
    "    const auto &__gea_child_node = __gea_tree.node(c.id());",
    "    const std::string __gea_child_tag = gea::embedded::ui::tagFromId(__gea_child_node.tag_id);",
    '    if (__gea_child_node.type == gea::embedded::ui::NodeType::Text || __gea_child_tag == "#comment" || __gea_child_tag == "#fragment" || __gea_child_tag == "#document-frag" || __gea_child_tag == "#document-fragment") continue;',
    "    kids.push_back(c);",
    "  }",
    "  return kids;",
    "}",
    "",
    "inline gea_cpp_value eventTargetValue(gea::framework::events::EventTarget target) {",
    "  return nodeValue(gea::embedded::ui::NodeHandle(target.id()));",
    "}",
    "",
    "inline gea_cpp_value touchPointValue(const gea::framework::events::TouchPoint &point) {",
    "  gea_cpp_value out = gea_cpp_value::empty_record();",
    '  out.record_set_literal("identifier", gea_cpp_value(static_cast<double>(point.identifier)));',
    '  out.record_set_literal("target", eventTargetValue(point.target));',
    '  out.record_set_literal("screenX", gea_cpp_value(static_cast<double>(point.screenX)));',
    '  out.record_set_literal("screenY", gea_cpp_value(static_cast<double>(point.screenY)));',
    '  out.record_set_literal("clientX", gea_cpp_value(static_cast<double>(point.clientX)));',
    '  out.record_set_literal("clientY", gea_cpp_value(static_cast<double>(point.clientY)));',
    '  out.record_set_literal("pageX", gea_cpp_value(static_cast<double>(point.pageX)));',
    '  out.record_set_literal("pageY", gea_cpp_value(static_cast<double>(point.pageY)));',
    "  return out;",
    "}",
    "",
    "inline gea_cpp_value touchPointArrayValue(const gea::framework::events::TouchPoint *points, int count) {",
    "  std::vector<gea_cpp_value> items;",
    "  if (count > 0) items.reserve(static_cast<std::size_t>(count));",
    "  for (int index = 0; index < count; ++index) items.push_back(touchPointValue(points[index]));",
    "  return gea_cpp_value::array(std::move(items));",
    "}",
    "",
    "inline gea_cpp_value pointerEventValue(gea::framework::events::PointerEvent &event) {",
    "  // LAZY event object: build only the field a handler actually reads. The old",
    "  // eager build allocated ~18 record fields PLUS the three touch arrays (each a",
    "  // record per point + a node-target) on EVERY dispatch, once per move",
    "  // (TouchMove) -- ~40 PSRAM allocations per move, measured at",
    "  // ~4ms, the dominant finger-down frame-rate cost -- when typical handlers read",
    "  // just clientX/clientY. The listener runs synchronously during dispatch, so",
    "  // capturing &event by reference is valid for the object lifetime; reads route",
    "  // through record_get -> object_get (value.cpp).",
    "  return gea_cpp_value::object(",
    "      reinterpret_cast<std::uintptr_t>(&event),",
    "      [&event](const std::string &key) -> gea_cpp_value {",
    '        if (key == "clientX") return gea_cpp_value(static_cast<double>(event.clientX));',
    '        if (key == "clientY") return gea_cpp_value(static_cast<double>(event.clientY));',
    '        if (key == "pointerId") return gea_cpp_value(static_cast<double>(event.pointerId));',
    '        if (key == "x") return gea_cpp_value(static_cast<double>(event.x));',
    '        if (key == "y") return gea_cpp_value(static_cast<double>(event.y));',
    '        if (key == "pageX") return gea_cpp_value(static_cast<double>(event.pageX));',
    '        if (key == "pageY") return gea_cpp_value(static_cast<double>(event.pageY));',
    '        if (key == "screenX") return gea_cpp_value(static_cast<double>(event.screenX));',
    '        if (key == "screenY") return gea_cpp_value(static_cast<double>(event.screenY));',
    '        if (key == "pressId") return gea_cpp_value(static_cast<double>(event.pressId));',
    '        if (key == "pressValue") return gea_cpp_value(static_cast<double>(event.pressValue));',
    '        if (key == "delta") return gea_cpp_value(static_cast<double>(event.delta));',
    '        if (key == "type") return gea_cpp_value(std::string(event.typeName()));',
    '        if (key == "target") return eventTargetValue(event.target);',
    '        if (key == "currentTarget") return eventTargetValue(event.currentTarget);',
    '        if (key == "touches") return touchPointArrayValue(event.touches, event.touchesLength);',
    '        if (key == "targetTouches") return touchPointArrayValue(event.targetTouches, event.targetTouchesLength);',
    '        if (key == "changedTouches") return touchPointArrayValue(event.changedTouches, event.changedTouchesLength);',
    '        if (key == "preventDefault") return gea_cpp_value([&event]() -> gea_cpp_value { event.preventDefault(); return gea_cpp_value::missing(); });',
    '        if (key == "stopPropagation") return gea_cpp_value([&event]() -> gea_cpp_value { event.stopPropagation(); return gea_cpp_value::missing(); });',
    "        return gea_cpp_value::missing();",
    "      });",
    "}",
    "",
    ...(usesCanvas
      ? [
          "inline gea::embedded::ui::CanvasRenderingContext2D getCanvasContext(const gea_cpp_value &node, const std::string &kind) {",
          '  if (kind != "2d") return gea::embedded::ui::CanvasRenderingContext2D();',
          "  return gea::embedded::ui::CanvasElement(nodeIdFromValue(node)).getContext2D();",
          "}",
          "",
          "inline gea::embedded::ui::CanvasRenderingContext2D getCanvasContext(const gea_cpp_value &node, const std::string &kind, const gea_cpp_value &) {",
          "  return getCanvasContext(node, kind);",
          "}",
          "",
          "inline gea::embedded::ui::CanvasRenderingContext2D getCanvasContext(gea::embedded::ui::NodeHandle node, const std::string &kind) {",
          '  if (kind != "2d") return gea::embedded::ui::CanvasRenderingContext2D();',
          "  return gea::embedded::ui::CanvasElement(node.id()).getContext2D();",
          "}",
          "",
          "inline gea::embedded::ui::CanvasRenderingContext2D getCanvasContext(gea::embedded::ui::NodeHandle node, const std::string &kind, const gea_cpp_value &) {",
          "  return getCanvasContext(node, kind);",
          "}",
          "",
          // geatsc stores a NULLABLE typed handle (`Component<T>`\'s `el: T | null`)
          // as std::optional<NodeHandle>. Without these unwrapping overloads the
          // optional binds the generic gea_cpp_value parameter instead, boxing an
          // OPAQUE object whose record carries no __gea_node_id — the context then
          // binds node -1 and every draw silently no-ops (maps: black screen).
          "inline gea::embedded::ui::CanvasRenderingContext2D getCanvasContext(const std::optional<gea::embedded::ui::NodeHandle> &node, const std::string &kind) {",
          "  return getCanvasContext(node.value_or(gea::embedded::ui::NodeHandle()), kind);",
          "}",
          "",
          "inline gea::embedded::ui::CanvasRenderingContext2D getCanvasContext(const std::optional<gea::embedded::ui::NodeHandle> &node, const std::string &kind, const gea_cpp_value &) {",
          "  return getCanvasContext(node.value_or(gea::embedded::ui::NodeHandle()), kind);",
          "}",
          "",
          "inline double canvasNumber(double value) { return value; }",
          "inline double canvasNumber(float value) { return static_cast<double>(value); }",
          "inline double canvasNumber(int8_t value) { return static_cast<double>(value); }",
          "inline double canvasNumber(uint8_t value) { return static_cast<double>(value); }",
          "inline double canvasNumber(int16_t value) { return static_cast<double>(value); }",
          "inline double canvasNumber(uint16_t value) { return static_cast<double>(value); }",
          "inline double canvasNumber(int value) { return static_cast<double>(value); }",
          "inline double canvasNumber(uint32_t value) { return static_cast<double>(value); }",
          "inline double canvasNumber(long long value) { return static_cast<double>(value); }",
          "inline double canvasNumber(const gea_f32 &value) { return static_cast<double>(value.v); }",
          "inline double canvasNumber(const gea_cpp_value &value) { return gea::runtime::coerce::to_number(value); }",
          // int32_t/uint32_t are long/unsigned long on some toolchains (xtensa newlib), so plain
          // `long` never matches the named overloads there — the arithmetic branch keeps every
          // scalar width native instead of falling through to a boxed gea_cpp_value round-trip.
          "template <typename T>",
          "inline double canvasNumber(const T &value) {",
          "  if constexpr (std::is_arithmetic_v<std::decay_t<T>>) return static_cast<double>(value);",
          "  else return gea::runtime::coerce::to_number(gea_cpp_key(value));",
          "}",
          "",
          "inline int canvasRound(double value) {",
          "  if (!std::isfinite(value)) return 0;",
          "  return static_cast<int>(value + (value >= 0 ? 0.5 : -0.5));",
          "}",
          "inline int canvasInt(int8_t value) { return static_cast<int>(value); }",
          "inline int canvasInt(uint8_t value) { return static_cast<int>(value); }",
          "inline int canvasInt(int16_t value) { return static_cast<int>(value); }",
          "inline int canvasInt(uint16_t value) { return static_cast<int>(value); }",
          "inline int canvasInt(int value) { return value; }",
          "inline int canvasInt(uint32_t value) { return static_cast<int>(value); }",
          "inline int canvasInt(long long value) { return static_cast<int>(value); }",
          "inline int canvasInt(double value) { return canvasRound(value); }",
          "inline int canvasInt(float value) { return canvasRound(static_cast<double>(value)); }",
          "inline int canvasInt(const gea_f32 &value) { return canvasRound(static_cast<double>(value.v)); }",
          "inline int canvasInt(const gea_cpp_value &value) { return canvasRound(gea::runtime::coerce::to_number(value)); }",
          "template <typename T>",
          "inline int canvasInt(const T &value) {",
          "  if constexpr (std::is_floating_point_v<std::decay_t<T>>) return canvasRound(static_cast<double>(value));",
          "  else if constexpr (std::is_arithmetic_v<std::decay_t<T>>) return static_cast<int>(value);",
          "  else return canvasRound(gea::runtime::coerce::to_number(gea_cpp_key(value)));",
          "}",
          "",
          "inline std::string canvasString(const std::string &value) { return value; }",
          "inline std::string canvasString(const char *value) { return value ? std::string(value) : std::string(); }",
          "inline std::string canvasString(const gea_cpp_value &value) { return gea_cpp_to_string(value); }",
          "template <typename T>",
          "inline std::string canvasString(const T &value) { return gea_cpp_to_string(gea_cpp_key(value)); }",
          "",
          "inline gea::framework::graphics::pixel::native_t canvasRgb565(gea::framework::graphics::pixel::NativeColor value) { return value.value; }",
          "inline gea::framework::graphics::pixel::native_t canvasRgb565(const gea_cpp_value &value) { return gea::framework::graphics::pixel::nativeFromRrggbbaa(static_cast<std::uint32_t>(gea::runtime::coerce::to_number(value))); }",
          "template <typename T>",
          "inline gea::framework::graphics::pixel::native_t canvasRgb565(const T &value) { return gea::framework::graphics::pixel::nativeFromRrggbbaa(static_cast<std::uint32_t>(canvasNumber(value))); }",
          "template <typename T>",
          "inline constexpr bool canvasColorValue = std::is_arithmetic_v<std::decay_t<T>> || std::is_same_v<std::decay_t<T>, gea::framework::graphics::pixel::NativeColor>;",
          "inline gea_cpp_value canvasAssignedValue(gea::framework::graphics::pixel::NativeColor value) { return gea_cpp_key(static_cast<double>(value.value)); }",
          "template <typename T>",
          "inline gea_cpp_value canvasAssignedValue(const T &value) { return gea_cpp_key(value); }",
          "",
          "inline int canvasImageId(const gea_cpp_value &value) {",
          "  if (value.kind == gea_cpp_value::kind_t::number) return canvasInt(value);",
          '  gea_cpp_value id = value.record_get_literal("id");',
          "  if (!id.is_nullish()) return canvasInt(id);",
          "  return -1;",
          "}",
          "template <typename T>",
          "inline int canvasImageId(const T &value) {",
          "  if constexpr (requires { value.id; }) return canvasInt(value.id);",
          "  else return canvasImageId(gea_cpp_key(value));",
          "}",
          "",
          "inline void canvasClear(gea::embedded::ui::CanvasRenderingContext2D &ctx) {",
          "  ctx.clear();",
          "}",
          "",
          "template <typename X, typename Y, typename W, typename H>",
          "inline void canvasClearRect(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X &x, const Y &y, const W &w, const H &h) {",
          "  ctx.clearRect(canvasInt(x), canvasInt(y), canvasInt(w), canvasInt(h));",
          "}",
          "",
          "template <typename X, typename Y, typename W, typename H>",
          "inline void canvasFillRect(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X &x, const Y &y, const W &w, const H &h) {",
          "  ctx.fillRect(canvasInt(x), canvasInt(y), canvasInt(w), canvasInt(h));",
          "}",
          "",
          "template <typename X, typename Y, typename W, typename H>",
          "inline void canvasStrokeRect(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X &x, const Y &y, const W &w, const H &h) {",
          "  ctx.strokeRect(canvasInt(x), canvasInt(y), canvasInt(w), canvasInt(h));",
          "}",
          "",
          "template <typename X, typename Y, typename R, typename Fill>",
          "inline void canvasFillCircle(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X &x, const Y &y, const R &r, const Fill &fill) {",
          "  if constexpr (canvasColorValue<Fill>) {",
          "    ctx.fillCircleRgb565(canvasInt(x), canvasInt(y), canvasInt(r), canvasRgb565(fill));",
          "  } else {",
          "    ctx.setFillStyle(canvasString(fill));",
          "    ctx.fillCircle(canvasInt(x), canvasInt(y), canvasInt(r));",
          "  }",
          "}",
          "",
          "template <typename X, typename Y, typename R>",
          "inline void canvasFillCircle(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X &x, const Y &y, const R &r, const gea_cpp_value &fill) {",
          "  if (fill.kind == gea_cpp_value::kind_t::number) {",
          "    ctx.fillCircleRgb565(canvasInt(x), canvasInt(y), canvasInt(r), canvasRgb565(fill.number));",
          "  } else {",
          "    ctx.setFillStyle(canvasString(fill));",
          "    ctx.fillCircle(canvasInt(x), canvasInt(y), canvasInt(r));",
          "  }",
          "}",
          "",
          "template <typename X, typename Y, typename R>",
          "inline void canvasStrokeCircle(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X &x, const Y &y, const R &r) {",
          "  ctx.strokeCircle(canvasInt(x), canvasInt(y), canvasInt(r));",
          "}",
          "",
          "template <typename X, typename Y, typename R, typename Fill>",
          "inline void canvasFillCircleRgb565(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X &x, const Y &y, const R &r, const Fill &fill) {",
          "  ctx.fillCircleRgb565(canvasInt(x), canvasInt(y), canvasInt(r), canvasRgb565(fill));",
          "}",
          "",
          "template <typename X0, typename Y0, typename X1, typename Y1, typename X2, typename Y2, typename Fill>",
          "inline void canvasFillTriangleRgb565(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X0 &x0, const Y0 &y0, const X1 &x1, const Y1 &y1, const X2 &x2, const Y2 &y2, const Fill &fill) {",
          "  ctx.fillTriangleRgb565(canvasInt(x0), canvasInt(y0), canvasInt(x1), canvasInt(y1), canvasInt(x2), canvasInt(y2), canvasRgb565(fill));",
          "}",
          "",
          "template <typename XS, typename YS, typename R, typename Colors>",
          "inline void canvasFillCirclesRgb565(gea::embedded::ui::CanvasRenderingContext2D &ctx, const XS &xs, const YS &ys, const R &r, const Colors &colors, int count = -1) {",
          "  // Convert each colour the same way the scalar path does (canvasRgb565):",
          "  // a plain number (e.g. from rgb() / Uint32Array) is authored 0xRRGGBBAA,",
          "  // while rgb565() values carry the NativeColor marker and pass through",
          "  // unchanged. `count` (>=0) limits to the first N triples so callers can",
          "  // pass a reused fixed-size scratch array without slicing; <0 uses all.",
          "  using __gea_color_t = std::decay_t<typename Colors::value_type>;",
          "  if constexpr (std::is_same_v<__gea_color_t, gea::framework::graphics::pixel::native_t> || std::is_same_v<__gea_color_t, gea::framework::graphics::pixel::NativeColor>) {",
          "    if constexpr (requires { xs.data(); ys.data(); colors.data(); } && std::is_same_v<std::decay_t<decltype(*xs.data())>, std::uint16_t> && std::is_same_v<std::decay_t<decltype(*ys.data())>, std::uint16_t>) {",
          "      // Pointer path. Passing the containers themselves binds a typed array",
          "      // through its implicit operator std::vector<Element>(), which heap-",
          "      // allocates and copies the FULL array on every call while ignoring",
          "      // `count` -- ~150us per call on esp32-s3 regardless of how many",
          "      // circles it carried.",
          "      std::size_t __gea_n = xs.size() < ys.size() ? xs.size() : ys.size();",
          "      if (colors.size() < __gea_n) __gea_n = colors.size();",
          "      if (count >= 0 && static_cast<std::size_t>(count) < __gea_n) __gea_n = static_cast<std::size_t>(count);",
          "      ctx.fillCirclesRgb565(xs.data(), ys.data(), canvasInt(r), colors.data(), static_cast<int>(__gea_n));",
          "    } else {",
          "      ctx.fillCirclesRgb565(xs, ys, canvasInt(r), colors, count);",
          "    }",
          "  } else {",
          "    std::size_t __gea_n = colors.size();",
          "    if (count >= 0 && static_cast<std::size_t>(count) < __gea_n) __gea_n = static_cast<std::size_t>(count);",
          "    std::vector<gea::framework::graphics::pixel::native_t> __gea_native_colors;",
          "    __gea_native_colors.reserve(__gea_n);",
          "    for (std::size_t __gea_i = 0; __gea_i < __gea_n; ++__gea_i) __gea_native_colors.push_back(canvasRgb565(colors[__gea_i]));",
          "    ctx.fillCirclesRgb565(xs, ys, canvasInt(r), __gea_native_colors, count);",
          "  }",
          "}",
          "",
          "template <typename XS, typename YS, typename R, typename Fill>",
          "inline void canvasFillCirclesRgb565Uniform(gea::embedded::ui::CanvasRenderingContext2D &ctx, const XS &xs, const YS &ys, const R &r, const Fill &fill) {",
          "  ctx.fillCirclesRgb565(xs, ys, canvasInt(r), canvasRgb565(fill));",
          "}",
          "",
          "template <typename X0s, typename Y0s, typename X1s, typename Y1s, typename X2s, typename Y2s, typename Colors, typename Order>",
          "inline void canvasFillTrianglesRgb565Sorted(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X0s &x0s, const Y0s &y0s, const X1s &x1s, const Y1s &y1s, const X2s &x2s, const Y2s &y2s, const Colors &colors, const Order &order, int count = -1) {",
          "  // Colours are authored 0xRRGGBBAA (plain numbers, e.g. a Uint32Array);",
          "  // the canvas layer converts during packing. `order` gives the paint",
          "  // (depth) order into the coordinate/colour arrays; `count` (>=0) limits",
          "  // to the first N entries so callers can reuse fixed-size scratch arrays.",
          "  ctx.fillTrianglesRgb565Sorted(x0s, y0s, x1s, y1s, x2s, y2s, colors, order, count);",
          "}",
          "",
          "inline void canvasBeginPath(gea::embedded::ui::CanvasRenderingContext2D &ctx) {",
          "  ctx.beginPath();",
          "}",
          "",
          "template <typename X, typename Y, typename R, typename Start, typename End>",
          "inline void canvasArc(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X &x, const Y &y, const R &r, const Start &start, const End &end) {",
          "  ctx.arc(canvasNumber(x), canvasNumber(y), canvasNumber(r), canvasNumber(start), canvasNumber(end));",
          "}",
          "",
          "template <typename X, typename Y>",
          "inline void canvasMoveTo(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X &x, const Y &y) {",
          "  ctx.moveTo(canvasNumber(x), canvasNumber(y));",
          "}",
          "",
          "template <typename X, typename Y>",
          "inline void canvasLineTo(gea::embedded::ui::CanvasRenderingContext2D &ctx, const X &x, const Y &y) {",
          "  ctx.lineTo(canvasNumber(x), canvasNumber(y));",
          "}",
          "",
          "inline void canvasClosePath(gea::embedded::ui::CanvasRenderingContext2D &ctx) {",
          "  ctx.closePath();",
          "}",
          "",
          "inline void canvasFill(gea::embedded::ui::CanvasRenderingContext2D &ctx) {",
          "  ctx.fill();",
          "}",
          "",
          "inline void canvasStroke(gea::embedded::ui::CanvasRenderingContext2D &ctx) {",
          "  ctx.stroke();",
          "}",
          "",
          "template <typename Text, typename X, typename Y>",
          "inline void canvasFillText(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Text &text, const X &x, const Y &y) {",
          "  ctx.fillText(canvasString(text), canvasInt(x), canvasInt(y));",
          "}",
          "",
          "template <typename Image, typename X, typename Y>",
          "inline void canvasDrawImage(gea::embedded::ui::CanvasRenderingContext2D ctx, const Image &image, const X &x, const Y &y) {",
          "  ctx.drawImage(canvasImageId(image), canvasInt(x), canvasInt(y));",
          "}",
          "",
          "template <typename Image, typename X, typename Y, typename W, typename H>",
          "inline void canvasDrawImage(gea::embedded::ui::CanvasRenderingContext2D ctx, const Image &image, const X &x, const Y &y, const W &w, const H &h) {",
          "  ctx.drawImage(canvasImageId(image), canvasInt(x), canvasInt(y), canvasInt(w), canvasInt(h));",
          "}",
          "",
          "template <typename Text>",
          "inline double canvasMeasureTextInkCenter(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Text &text) {",
          "  return ctx.measureTextInkCenter(canvasString(text));",
          "}",
          "",
          "template <typename Text>",
          "inline double canvasMeasureText(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Text &text) {",
          "  return ctx.measureText(canvasString(text));",
          "}",
          "",
          "template <typename Image, typename X, typename Y, typename W, typename H>",
          "inline void canvasDrawImageCircle(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Image &image, const X &x, const Y &y, const W &w, const H &h) {",
          "  ctx.drawImageCircle(canvasImageId(image), canvasInt(x), canvasInt(y), canvasInt(w), canvasInt(h));",
          "}",
          "",
          "template <typename Image, typename X, typename Y, typename W, typename H>",
          "inline void canvasDrawImageRotated90CW(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Image &image, const X &x, const Y &y, const W &w, const H &h) {",
          "  ctx.drawImageRotated90CW(canvasImageId(image), canvasInt(x), canvasInt(y), canvasInt(w), canvasInt(h));",
          "}",
          "",
          "template <typename Image, typename X, typename Y, typename W>",
          "inline void canvasDrawImageTiledX(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Image &image, const X &x, const Y &y, const W &w) {",
          "  ctx.drawImageTiledX(canvasImageId(image), canvasInt(x), canvasInt(y), canvasInt(w));",
          "}",
          "",
          "inline void canvasFlush(gea::embedded::ui::CanvasRenderingContext2D &ctx) {",
          "  ctx.flush();",
          "}",
          "",
          "inline void canvasBeginBatch(gea::embedded::ui::CanvasRenderingContext2D &ctx) {",
          "  ctx.beginBatch();",
          "}",
          "",
          "inline void canvasEndBatch(gea::embedded::ui::CanvasRenderingContext2D &ctx) {",
          "  ctx.endBatch();",
          "}",
          "",
          "template <typename Value>",
          "inline gea_cpp_value canvasSetFillStyle(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Value &value) {",
          "  if constexpr (canvasColorValue<Value>) ctx.setFillStyleRgb565(canvasRgb565(value));",
          "  else ctx.setFillStyle(canvasString(value));",
          "  return canvasAssignedValue(value);",
          "}",
          "",
          "inline gea_cpp_value canvasSetFillStyle(gea::embedded::ui::CanvasRenderingContext2D &ctx, const gea_cpp_value &value) {",
          "  if (value.kind == gea_cpp_value::kind_t::number) ctx.setFillStyleRgb565(canvasRgb565(value.number));",
          "  else ctx.setFillStyle(canvasString(value));",
          "  return value;",
          "}",
          "",
          "inline gea_cpp_value canvasSetFillStyleRgb565(gea::embedded::ui::CanvasRenderingContext2D &ctx, gea::framework::graphics::pixel::native_t value) {",
          "  ctx.setFillStyleRgb565(value);",
          "  return gea_cpp_value(static_cast<double>(value));",
          "}",
          "",
          "template <typename Value>",
          "inline gea_cpp_value canvasSetStrokeStyle(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Value &value) {",
          "  if constexpr (canvasColorValue<Value>) ctx.setStrokeStyleRgb565(canvasRgb565(value));",
          "  else ctx.setStrokeStyle(canvasString(value));",
          "  return canvasAssignedValue(value);",
          "}",
          "",
          "inline gea_cpp_value canvasSetStrokeStyle(gea::embedded::ui::CanvasRenderingContext2D &ctx, const gea_cpp_value &value) {",
          "  if (value.kind == gea_cpp_value::kind_t::number) ctx.setStrokeStyleRgb565(canvasRgb565(value.number));",
          "  else ctx.setStrokeStyle(canvasString(value));",
          "  return value;",
          "}",
          "",
          "template <typename Value>",
          "inline gea_cpp_value canvasSetGlobalAlpha(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Value &value) {",
          "  ctx.setGlobalAlpha(canvasNumber(value));",
          "  return gea_cpp_key(value);",
          "}",
          "",
          "template <typename Value>",
          "inline gea_cpp_value canvasSetLineWidth(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Value &value) {",
          "  ctx.setLineWidth(canvasNumber(value));",
          "  return gea_cpp_key(value);",
          "}",
          "",
          "template <typename Value>",
          "inline gea_cpp_value canvasSetFont(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Value &value) {",
          "  ctx.setFont(canvasString(value));",
          "  return gea_cpp_key(value);",
          "}",
          "",
          "template <typename Value>",
          "inline gea_cpp_value canvasSetTextBaseline(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Value &value) {",
          "  ctx.setTextBaseline(canvasString(value));",
          "  return gea_cpp_key(value);",
          "}",
          "",
          "template <typename Value>",
          "inline gea_cpp_value canvasSetTextAlign(gea::embedded::ui::CanvasRenderingContext2D &ctx, const Value &value) {",
          "  ctx.setTextAlign(canvasString(value));",
          "  return gea_cpp_key(value);",
          "}",
          "",
        ]
      : []),
    "// (global scope) Boxing a NodeHandle must produce the FULL node value with",
    "// the DOM method table (cloneNode/firstChild/childNodes/appendChild/...).",
    "// Compiled templates pass typed handles across boxed boundaries (appendChild",
    "// on the mount root, the component GEA_ELEMENT slot); the generic templated",
    "// gea_cpp_key boxes them as opaque objects that answer NO node protocol, so",
    "// the tree never mounts and the screen stays black. Declared OUTSIDE",
    "// namespace gea_ir: an in-namespace overload would hide the global",
    "// gea_cpp_key overload set from every later in-namespace helper.",
    "}  // namespace gea_ir (reopened below)",
    "static gea_cpp_value gea_cpp_key(const gea::embedded::ui::NodeHandle &node) {",
    "  return gea_ir::nodeValue(node);",
    "}",
    "// Boxing bridge for native pointer events: lets geatsc's duck-binding",
    "// adapt a `std::function<void(gea_cpp_value)>` record slot to a host",
    "// `EventListener` (std::function<void(PointerEvent&)>) by wrapping the",
    "// callback in a generic lambda that boxes the event (`document as",
    "// EventSourceLike` in canvas-runtime apps).",
    "static gea_cpp_value gea_cpp_key(gea::framework::events::PointerEvent &event) {",
    "  return gea_ir::pointerEventValue(event);",
    "}",
    "namespace gea_ir {",
    "",
  ];
}

function domStyleInteropSource(): string[] {
  return [
    "inline int styleDisplayValue(const std::string &value) {",
    '  if (value == "none") return gea::embedded::ui::kDisplayNone;',
    '  if (value == "grid") return gea::embedded::ui::kDisplayGrid;',
    '  if (value == "flex") return gea::embedded::ui::kDisplayFlex;',
    "  return gea::embedded::ui::kDisplayBlock;",
    "}",
    "",
    "inline int styleDisplayValue(const char *value) {",
    "  return styleDisplayValue(value ? std::string(value) : std::string());",
    "}",
    "",
    "inline int styleDisplayValue(const gea_cpp_value &value) {",
    "  return styleDisplayValue(value.is_nullish() ? std::string() : gea_cpp_to_string(value));",
    "}",
    "",
    "template <typename T>",
    "inline int styleDisplayValue(const T &value) {",
    "  if constexpr (std::is_arithmetic_v<std::decay_t<T>>) return static_cast<int>(value);",
    "  else return styleDisplayValue(gea_cpp_to_string(gea_cpp_key(value)));",
    "}",
  ];
}

function cppNamespace(value: string): string {
  const parts = value
    .split("::")
    .map((part) => sanitizeCppIdentifier(part.trim()))
    .filter(Boolean);
  return parts.length > 0 ? parts.join("::") : "gea::framework::app::generated";
}
