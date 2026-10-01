import ts from 'typescript'
import path from 'node:path'

// Each shape bounds a possible string without materializing its values. The
// boundary bits let `slot-${number}` stay one token, while preserving spaces
// introduced by literals or either arm of a conditional.
type Shape = { count: number; start: boolean; end: boolean; empty: boolean }
const atom: Shape = { count: 1, start: true, end: true, empty: false }
const space = /[\x00-\x20]/
function literal(value: string): Shape {
  const parts = value.split(/[\x00-\x20]+/).filter(Boolean)
  return { count: Math.min(4, parts.length), start: !!value && !space.test(value[0]), end: !!value && !space.test(value.at(-1)!), empty: !value }
}
function unique(shapes: Shape[]): Shape[] {
  return [...new Map(shapes.map(s => [`${s.count}/${s.start}/${s.end}/${s.empty}`, s])).values()]
}
function join(left: Shape[], right: Shape[]): Shape[] {
  return unique(left.flatMap(a => right.map(b => ({
    count: Math.min(4, a.count + b.count - (a.end && b.start ? 1 : 0)),
    start: a.empty ? b.start : a.start,
    end: b.empty ? a.end : b.end,
    empty: a.empty && b.empty,
  }))))
}

// A positive proof bounds every class assignment; unknown mutation invalidates
// it. Callers version inline-capacity tuning and overflow elimination separately.
export function analyzeClassCapacity(files: Map<string, string>, opaque: boolean): number | undefined {
  if (opaque) return undefined
  const sources = new Map([...files].filter(([file]) => !/\.css$/i.test(file)).map(([file, text]) =>
    [file, ts.createSourceFile(file, text, ts.ScriptTarget.Latest, true, /\.[jt]sx$/i.test(file) ? ts.ScriptKind.TSX : ts.ScriptKind.TS)]))
  let checker: ts.TypeChecker | undefined
  const getChecker = (): ts.TypeChecker => {
    if (checker) return checker
    // Use the already-discovered graph only. External packages cannot provide
    // a numeric proof here, and this pass neither loads libraries nor emits.
    const host: ts.CompilerHost = {
      getSourceFile: file => sources.get(path.resolve(file)),
      getDefaultLibFileName: () => '', writeFile: () => {},
      getCurrentDirectory: () => '/', getDirectories: () => [],
      getCanonicalFileName: file => file, useCaseSensitiveFileNames: () => true,
      getNewLine: () => '\n', fileExists: file => sources.has(path.resolve(file)),
      readFile: file => files.get(path.resolve(file)),
      resolveModuleNames: (names, containingFile) => names.map(name => {
        if (!name.startsWith('.')) return undefined
        const base = path.resolve(path.dirname(containingFile), name)
        const candidates = [base, ...['.ts', '.tsx', '.js', '.jsx', '.mjs'].map(ext => base + ext),
          ...['index.ts', 'index.tsx', 'index.js', 'index.jsx'].map(file => path.join(base, file))]
        const resolved = candidates.find(file => sources.has(file))
        if (!resolved) return undefined
        return { resolvedFileName: resolved, extension: /\.tsx$/i.test(resolved) ? ts.Extension.Tsx : /\.jsx$/i.test(resolved) ? ts.Extension.Jsx : /\.[cm]?js$/i.test(resolved) ? ts.Extension.Js : ts.Extension.Ts }
      }),
    }
    checker = ts.createProgram([...sources.keys()], { noLib: true, target: ts.ScriptTarget.ESNext, module: ts.ModuleKind.ESNext, moduleResolution: ts.ModuleResolutionKind.Bundler, jsx: ts.JsxEmit.Preserve, strict: true, allowJs: true }, host).getTypeChecker()
    return checker
  }
  const scalarShapes = (node: ts.Expression): Shape[] | undefined => {
    if (ts.isParenthesizedExpression(node)) return scalarShapes(node.expression)
    // Assertions are not a source proof. Unknown values must not acquire a
    // smaller class capacity merely because a caller asserted a type.
    if (ts.isAsExpression(node) || ts.isTypeAssertionExpression(node) || ts.isNonNullExpression(node)) return undefined
    if (ts.isStringLiteralLike(node)) return [literal(node.text)]
    if (ts.isNumericLiteral(node) || ts.isBigIntLiteral(node)) return [atom]
    if (ts.isConditionalExpression(node)) {
      const a = scalarShapes(node.whenTrue), b = scalarShapes(node.whenFalse)
      return a && b ? unique([...a, ...b]) : undefined
    }
    if (ts.isTemplateExpression(node)) {
      let result = [literal(node.head.text)]
      for (const part of node.templateSpans) {
        const value = scalarShapes(part.expression)
        if (!value) return undefined
        result = join(join(result, value), [literal(part.literal.text)])
      }
      return result
    }
    if (ts.isBinaryExpression(node) && node.operatorToken.kind === ts.SyntaxKind.PlusToken) {
      const a = scalarShapes(node.left), b = scalarShapes(node.right)
      return a && b ? join(a, b) : undefined
    }
    const type = getChecker().getTypeAtLocation(node)
    const parts = type.isUnion() ? type.types : [type]
    if (parts.every(t => !!(t.flags & (ts.TypeFlags.NumberLike | ts.TypeFlags.BigIntLike | ts.TypeFlags.BooleanLike)))) return [atom]
    return undefined
  }
  const classCount = (node: ts.Expression): number | undefined => {
    if (ts.isParenthesizedExpression(node)) return classCount(node.expression)
    if (ts.isObjectLiteralExpression(node)) {
      let count = 0
      for (const property of node.properties) {
        if (!ts.isPropertyAssignment(property) && !ts.isShorthandPropertyAssignment(property)) return undefined
        if (ts.isComputedPropertyName(property.name)) return undefined
        const name = property.name
        if (!ts.isIdentifier(name) && !ts.isStringLiteralLike(name) && !ts.isNumericLiteral(name)) return undefined
        count += literal(name.text).count
      }
      return count
    }
    if (ts.isConditionalExpression(node)) {
      const a = classCount(node.whenTrue), b = classCount(node.whenFalse)
      return a !== undefined && b !== undefined ? Math.max(a, b) : undefined
    }
    const shapes = scalarShapes(node)
    return shapes && Math.max(...shapes.map(s => s.count))
  }
  let maximum = 0, unknown = false
  const dangerous = /^(?:className|classList|attributes|getAttributeNode|setAttributeNode|setAttribute|setAttributeNS|setClassName|innerHTML|outerHTML|insertAdjacentHTML|DOMParser|eval|Function|Proxy|Reflect|Object|VirtualList|VirtualListElement|createList|createVirtualList)$/
  const visit = (node: ts.Node): void => {
    if (unknown) return
    // Casts can hide arbitrary class strings through aliases in another file.
    if (ts.isAsExpression(node) || ts.isTypeAssertionExpression(node) || ts.isNonNullExpression(node)) unknown = true
    if (ts.isIdentifier(node) && dangerous.test(node.text) && !(ts.isJsxAttribute(node.parent) && node.parent.name === node)) unknown = true
    if (ts.isStringLiteralLike(node) && (dangerous.test(node.text) || /^(?:class|list|virtual-list)$/.test(node.text)) && !ts.isJsxAttribute(node.parent)) unknown = true
    if (ts.isJsxSpreadAttribute(node)) unknown = true
    if (ts.isJsxAttribute(node) && (ts.isJsxNamespacedName(node.name) || node.name.getText() === 'classList')) unknown = true
    if (ts.isJsxOpeningElement(node) || ts.isJsxSelfClosingElement(node)) {
      if (/^(?:list|virtual-list|input|textarea|select)$/i.test(node.tagName.getText())) unknown = true
    }
    if (ts.isPropertyAccessExpression(node) && ts.isIdentifier(node.expression) && node.expression.text === 'Object' && /^(?:assign|defineProperty|defineProperties|setPrototypeOf)$/.test(node.name.text)) unknown = true
    // A computed DOM member may name className/classList or a mutation method.
    // Numeric indexes cannot name those members; ordinary array access is safe.
    if (ts.isElementAccessExpression(node) && (!node.argumentExpression ||
        (!ts.isStringLiteralLike(node.argumentExpression) && !(getChecker().getTypeAtLocation(node.argumentExpression).flags & ts.TypeFlags.NumberLike)))) unknown = true
    if (ts.isJsxAttribute(node) && /^(?:class|className)$/.test(node.name.getText())) {
      const init = node.initializer
      const count = !init ? 0 : ts.isStringLiteral(init) ? literal(init.text).count
        : ts.isJsxExpression(init) && init.expression ? classCount(init.expression) : undefined
      if (count === undefined) unknown = true
      else maximum = Math.max(maximum, count)
    }
    ts.forEachChild(node, visit)
  }
  for (const source of sources.values()) {
    if ((source as ts.SourceFile & { parseDiagnostics?: readonly ts.Diagnostic[] }).parseDiagnostics?.length) return undefined
    visit(source)
  }
  return unknown || maximum >= 4 ? undefined : Math.max(1, maximum)
}
