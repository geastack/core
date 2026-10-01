import ts from 'typescript'

// This proof is intentionally narrower than "no JSX". The canvas frame/boot
// path omits document refresh and service callbacks, so every executable import
// and free global must be understood before that path may be selected.
const canvasDisplayMembers = new Set(['ctx', 'width', 'height', 'getDevicePixelRatio', 'getFrameIntervalMs', 'setFrameIntervalMs', 'getFrameRate', 'setFrameRate', 'setAA', 'setFlushConfig'])
const canvasHostExports = new Set(['Display', 'rgb'])
const pureGlobals = new Set([
  'undefined', 'NaN', 'Infinity', 'Math', 'Number', 'String', 'Boolean', 'BigInt',
  'Object', 'Array', 'ArrayBuffer', 'SharedArrayBuffer', 'DataView', 'Map', 'Set',
  'WeakMap', 'WeakSet', 'Symbol', 'Date', 'RegExp', 'JSON', 'Promise',
  'Int8Array', 'Uint8Array', 'Uint8ClampedArray', 'Int16Array', 'Uint16Array',
  'Int32Array', 'Uint32Array', 'Float32Array', 'Float64Array', 'BigInt64Array', 'BigUint64Array',
  'Error', 'TypeError', 'RangeError', 'ReferenceError', 'SyntaxError', 'URIError', 'EvalError',
  'parseInt', 'parseFloat', 'isNaN', 'isFinite', 'decodeURI', 'decodeURIComponent',
  'encodeURI', 'encodeURIComponent', 'console', 'performance',
  'requestAnimationFrame', 'setTimeout', 'clearTimeout', 'setInterval', 'clearInterval',
])
const hostModule = /^(?:gea-embedded|@geastack\/(?:core|engine)|@geajs\/core)(?:\/|$)/
const canvasModule = /^(?:gea-embedded|@geastack\/core)$/

export function inferCanvasOnly(sources: ReadonlyMap<string, string>, unknown: boolean): boolean {
  if (unknown || !sources.size) return false
  const files = new Map<string, ts.SourceFile>()
  for (const [file, text] of sources) {
    if (!/\.[cm]?[jt]sx?$/.test(file)) return false
    const source = ts.createSourceFile(file, text, ts.ScriptTarget.Latest, true,
      /\.[jt]sx$/.test(file) ? ts.ScriptKind.TSX : /\.[cm]?js$/.test(file) ? ts.ScriptKind.JS : ts.ScriptKind.TS)
    if ((source as ts.SourceFile & { parseDiagnostics?: readonly ts.Diagnostic[] }).parseDiagnostics?.length) return false
    files.set(file, source)
  }
  // Binding local names avoids both a host-global blacklist (which misses new
  // APIs) and false positives for ordinary app methods named fetch/document.
  // No library or filesystem resolution: discovery already owns the graph.
  const options: ts.CompilerOptions = { noLib: true, noResolve: true, types: [], allowJs: true, target: ts.ScriptTarget.Latest }
  const host = ts.createCompilerHost(options)
  host.getSourceFile = file => files.get(file)
  host.resolveModuleNames = names => names.map(() => undefined)
  host.directoryExists = () => false
  host.getDirectories = () => []
  host.fileExists = file => files.has(file)
  host.readFile = file => sources.get(file)
  const checker = ts.createProgram([...files.keys()], options, host).getTypeChecker()
  let safe = true
  let display = false
  const visit = (node: ts.Node): void => {
    if (ts.canHaveModifiers(node) && ts.getModifiers(node)?.some(m => m.kind === ts.SyntaxKind.DeclareKeyword)) return
    if (!safe || ts.isTypeNode(node) || ts.isInterfaceDeclaration(node) || ts.isTypeAliasDeclaration(node)) return
    if (ts.isJsxElement(node) || ts.isJsxSelfClosingElement(node) || ts.isJsxFragment(node)) { safe = false; return }
    if (ts.isImportDeclaration(node)) {
      if (node.importClause?.isTypeOnly) return
      const module = ts.isStringLiteral(node.moduleSpecifier) ? node.moduleSpecifier.text : ''
      if (hostModule.test(module)) {
        const clause = node.importClause
        const names = clause?.namedBindings
        if (!canvasModule.test(module) || !clause || clause.name || !names || !ts.isNamedImports(names)) { safe = false; return }
        for (const item of names.elements) {
          if (item.isTypeOnly) continue
          const name = (item.propertyName ?? item.name).text
          if (!canvasHostExports.has(name)) safe = false
          if (name === 'Display') display = true
        }
      }
      return
    }
    if (ts.isExportDeclaration(node)) {
      if (!node.isTypeOnly && node.moduleSpecifier && ts.isStringLiteral(node.moduleSpecifier) && hostModule.test(node.moduleSpecifier.text)) safe = false
      return
    }
    if (ts.isImportEqualsDeclaration(node) ||
        (ts.isCallExpression(node) && (node.expression.kind === ts.SyntaxKind.ImportKeyword ||
          (ts.isIdentifier(node.expression) && ['require', 'eval', 'Function'].includes(node.expression.text))))) {
      safe = false; return
    }
    if (ts.isIdentifier(node) && isValueReference(node)) {
      const symbol = ts.isShorthandPropertyAssignment(node.parent)
        ? checker.getShorthandAssignmentValueSymbol(node.parent) : checker.getSymbolAtLocation(node)
      const importedDisplay = symbol?.declarations?.find(d => ts.isImportSpecifier(d) && (d.propertyName ?? d.name).text === 'Display' && ts.isStringLiteral(d.parent.parent.parent.moduleSpecifier) && canvasModule.test(d.parent.parent.parent.moduleSpecifier.text))
      if (importedDisplay) {
        const parent = node.parent
        if (!ts.isPropertyAccessExpression(parent) || parent.expression !== node || !canvasDisplayMembers.has(parent.name.text)) safe = false
      }
      if (!symbol?.declarations?.length || symbol.declarations.some(isAmbient)) {
        if (node.text === 'window') {
          const parent = node.parent
          if (!ts.isPropertyAccessExpression(parent) || parent.expression !== node ||
              !['innerWidth', 'innerHeight', 'devicePixelRatio'].includes(parent.name.text)) safe = false
        } else if (!pureGlobals.has(node.text)) safe = false
      }
    }
    ts.forEachChild(node, visit)
  }
  for (const source of files.values()) visit(source)
  return safe && display
}

function isValueReference(node: ts.Identifier): boolean {
  const parent = node.parent
  if (ts.isPropertyAccessExpression(parent) && parent.name === node) return false
  if (ts.isBindingElement(parent) && parent.propertyName === node) return false
  if (ts.isLabeledStatement(parent) || ts.isBreakStatement(parent) || ts.isContinueStatement(parent)) return false
  // Declaration/property names are not reads; shorthand properties are.
  if ('name' in parent && parent.name === node && !ts.isShorthandPropertyAssignment(parent)) return false
  return true
}

function isAmbient(node: ts.Node): boolean {
  for (let current: ts.Node | undefined = node; current; current = current.parent) {
    if (ts.isSourceFile(current) && current.isDeclarationFile) return true
    if (ts.canHaveModifiers(current) && ts.getModifiers(current)?.some(m => m.kind === ts.SyntaxKind.DeclareKeyword)) return true
  }
  return false
}
