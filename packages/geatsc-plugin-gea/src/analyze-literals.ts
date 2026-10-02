import ts from 'typescript'
import path from 'node:path'

// A closed-source, finite literal proof for style values. It deliberately uses
// all writes to a property name, even across unrelated objects: ambiguity keeps
// storage. Types alone never certify colour values. Mutable palettes must not
// escape their declaration/import/indexed-read paths.
export function sourceLiteralResolver(files: Map<string, string>, initiallyOpaque: boolean) {
  const sources = new Map([...files].filter(([file]) => !/\.css$/i.test(file)).map(([file, text]) =>
    [file, ts.createSourceFile(file, text, ts.ScriptTarget.Latest, true, /\.[jt]sx$/i.test(file) ? ts.ScriptKind.TSX : ts.ScriptKind.TS)]))
  const variables = new Map<string, ts.Expression | undefined>()
  const properties = new Map<string, Array<ts.Expression | undefined>>()
  const arrays = new Set<string>(), escapedArrays = new Set<string>()
  const calledMembers = new Set<string>()
  let opaque = initiallyOpaque
  let numericOpaque = initiallyOpaque
  const nameOf = (node: ts.PropertyName | undefined): string | undefined =>
    node && (ts.isIdentifier(node) || ts.isStringLiteralLike(node) || ts.isNumericLiteral(node)) ? node.text : undefined
  const member = (node: ts.Expression): string | undefined => ts.isPropertyAccessExpression(node) ? node.name.text
    : ts.isElementAccessExpression(node) && node.argumentExpression && ts.isStringLiteralLike(node.argumentExpression) ? node.argumentExpression.text : undefined
  const addProperty = (name: string | undefined, value: ts.Expression | undefined): void => {
    if (name === undefined) { opaque = true; return }
    const values = properties.get(name) ?? []; values.push(value); properties.set(name, values)
  }
  const collect = (node: ts.Node): void => {
    if (ts.isCallExpression(node)) { const name = member(node.expression); if (name !== undefined) calledMembers.add(name) }
    if (ts.isVariableDeclaration(node) && ts.isIdentifier(node.name)) {
      const name = node.name.text
      const immutable = ts.isVariableDeclarationList(node.parent) && (node.parent.flags & ts.NodeFlags.Const)
      variables.set(name, variables.has(name) || !immutable ? undefined : node.initializer)
      if (node.initializer && ts.isArrayLiteralExpression(node.initializer)) arrays.add(name)
    }
    if (ts.isParameter(node) && ts.isIdentifier(node.name)) variables.set(node.name.text, undefined)
    if (ts.isPropertyDeclaration(node) || ts.isPropertyAssignment(node)) addProperty(nameOf(node.name), node.initializer)
    if (ts.isGetAccessorDeclaration(node)) {
      const returns: Array<ts.Expression | undefined> = []
      const visit = (child: ts.Node): void => { if (ts.isReturnStatement(child)) returns.push(child.expression); ts.forEachChild(child, visit) }
      if (node.body) visit(node.body)
      for (const value of returns.length ? returns : [undefined]) addProperty(nameOf(node.name), value)
    }
    ts.forEachChild(node, collect)
  }
  for (const source of sources.values()) {
    if ((source as ts.SourceFile & { parseDiagnostics?: readonly ts.Diagnostic[] }).parseDiagnostics?.length) opaque = true
    collect(source)
  }
  // Native numeric aliases and the collection signature needed to type a
  // JSX map callback. No imported implementation is trusted by this type fact.
  const intrinsicFile = '/__gea_numeric_style_intrinsics.d.ts'
  const intrinsic = ts.createSourceFile(intrinsicFile, `
    type int = number; type float = number; type double = number;
    interface Array<T> { [n: number]: T; length: number; map<U>(fn: (value: T, index: number, array: T[]) => U): U[]; }
    interface ReadonlyArray<T> { readonly [n: number]: T; readonly length: number; map<U>(fn: (value: T, index: number, array: readonly T[]) => U): U[]; }
  `, ts.ScriptTarget.Latest, true)
  let checker: ts.TypeChecker | undefined
  const typeChecker = (): ts.TypeChecker => {
    if (!checker) {
      const host: ts.CompilerHost = {
        getSourceFile: file => file === intrinsicFile ? intrinsic : sources.get(path.resolve(file)), getDefaultLibFileName: () => '', writeFile: () => {},
        getCurrentDirectory: () => '/', getDirectories: () => [], getCanonicalFileName: file => file,
        useCaseSensitiveFileNames: () => true, getNewLine: () => '\n',
        fileExists: file => sources.has(path.resolve(file)), readFile: file => files.get(path.resolve(file)),
        resolveModuleNames: (names, from) => names.map(name => {
          if (!name.startsWith('.')) return undefined
          const base = path.resolve(path.dirname(from), name.replace(/\.js$/, ''))
          const resolved = [base, ...['.ts', '.tsx', '.js', '.jsx'].map(ext => base + ext), ...['index.ts', 'index.tsx'].map(file => path.join(base, file))].find(file => sources.has(file))
          return resolved ? { resolvedFileName: resolved, extension: /\.tsx$/.test(resolved) ? ts.Extension.Tsx : ts.Extension.Ts } : undefined
        }),
      }
      checker = ts.createProgram([...sources.keys(), intrinsicFile], { noLib: true, target: ts.ScriptTarget.ESNext, module: ts.ModuleKind.ESNext, moduleResolution: ts.ModuleResolutionKind.Bundler, jsx: ts.JsxEmit.Preserve, strict: true, allowJs: true }, host).getTypeChecker()
    }
    return checker
  }
  const numeric = (node: ts.Expression): boolean => !!(typeChecker().getTypeAtLocation(node).flags & ts.TypeFlags.NumberLike)
  const declarationOf = (node: ts.Node): ts.VariableDeclaration | undefined => {
    let symbol = ts.isShorthandPropertyAssignment(node.parent) ? typeChecker().getShorthandAssignmentValueSymbol(node.parent) : typeChecker().getSymbolAtLocation(node)
    if (symbol && (symbol.flags & ts.SymbolFlags.Alias)) symbol = typeChecker().getAliasedSymbol(symbol)
    const declaration = symbol?.valueDeclaration
    return declaration && ts.isVariableDeclaration(declaration) && ts.isIdentifier(declaration.name) &&
      sources.has(declaration.getSourceFile().fileName) && ts.isVariableDeclarationList(declaration.parent) &&
      (declaration.parent.flags & ts.NodeFlags.Const) ? declaration : undefined
  }
  const localReceiver = (node: ts.Expression, seen = new Set<ts.Node>()): boolean => {
    if (seen.has(node)) return false
    seen.add(node)
    if (ts.isParenthesizedExpression(node)) return localReceiver(node.expression, seen)
    if (node.kind === ts.SyntaxKind.ThisKeyword) return true
    if (ts.isObjectLiteralExpression(node)) return true
    if (ts.isIdentifier(node)) {
      const declaration = declarationOf(node)
      if (declaration?.initializer) return localReceiver(declaration.initializer, seen)
      // A default-exported instance has an ExportAssignment declaration,
      // rather than a const variable. Follow its authored initializer too.
      let symbol = typeChecker().getSymbolAtLocation(node)
      if (symbol && (symbol.flags & ts.SymbolFlags.Alias)) symbol = typeChecker().getAliasedSymbol(symbol)
      const exported = symbol?.valueDeclaration
      return !!exported && ts.isExportAssignment(exported) &&
        sources.has(exported.getSourceFile().fileName) && localReceiver(exported.expression, seen)
    }
    if (ts.isNewExpression(node)) {
      let symbol = typeChecker().getSymbolAtLocation(node.expression)
      if (symbol && (symbol.flags & ts.SymbolFlags.Alias)) symbol = typeChecker().getAliasedSymbol(symbol)
      const declaration = symbol?.valueDeclaration
      if (!declaration || !ts.isClassDeclaration(declaration) || !sources.has(declaration.getSourceFile().fileName)) return false
      // A constructor returning another object breaks the local-instance proof.
      let returnsObject = false
      const visit = (child: ts.Node): void => { if (ts.isReturnStatement(child) && child.expression) returnsObject = true; ts.forEachChild(child, visit) }
      for (const member of declaration.members) if (ts.isConstructorDeclaration(member)) visit(member)
      return !returnsObject
    }
    return false
  }
  const writes = (node: ts.Node): void => {
    // Follow receiver references, not just call argument shapes: hiding an
    // instance inside an array/object before passing it out is still an escape.
    if ((ts.isIdentifier(node) && localReceiver(node)) || node.kind === ts.SyntaxKind.ThisKeyword) {
      const parent = node.parent
      const memberRead = (ts.isPropertyAccessExpression(parent) || ts.isElementAccessExpression(parent)) && parent.expression === node
      const declaration = ts.isVariableDeclaration(parent) && parent.name === node
      const immutableAlias = ts.isVariableDeclaration(parent) && parent.initializer === node && ts.isVariableDeclarationList(parent.parent) && (parent.parent.flags & ts.NodeFlags.Const)
      const imported = ts.isImportSpecifier(parent) || ts.isExportSpecifier(parent)
      if (!memberRead && !declaration && !immutableAlias && !imported) opaque = true
    }
    // Passing or publishing a mutable local receiver lets opaque code replace
    // a field without a visible assignment. Keep dynamic-value storage rather
    // than relying on the apparent class type at the later read.
    if ((ts.isCallExpression(node) || ts.isNewExpression(node)) && node.arguments?.some(argument => localReceiver(argument))) opaque = true
    if (ts.isReturnStatement(node) && node.expression && localReceiver(node.expression)) opaque = true
    if (ts.isPropertyAccessExpression(node) && /^(?:call|apply|bind)$/.test(node.name.text)) opaque = true
    if (ts.isCallExpression(node) && (ts.isPropertyAccessExpression(node.expression) || ts.isElementAccessExpression(node.expression)) && localReceiver(node.expression.expression)) {
      const declaration = typeChecker().getResolvedSignature(node)?.declaration
      // An opaque method receives the object implicitly through `this`.
      if (!declaration || !sources.has(declaration.getSourceFile().fileName) ||
          !(ts.isMethodDeclaration(declaration) || ts.isFunctionDeclaration(declaration) || ts.isFunctionExpression(declaration) || ts.isArrowFunction(declaration))) opaque = true
    }
    if (ts.isBinaryExpression(node) && node.operatorToken.kind === ts.SyntaxKind.EqualsToken &&
        (ts.isPropertyAccessExpression(node.left) || ts.isElementAccessExpression(node.left)) && localReceiver(node.right)) opaque = true
    if (ts.isAsExpression(node) || ts.isTypeAssertionExpression(node) || ts.isNonNullExpression(node) ||
        ts.isSpreadAssignment(node) || ts.isSpreadElement(node)) { opaque = true; numericOpaque = true }
    if (ts.isIdentifier(node) && /^(?:Object|Reflect|Proxy|eval|Function)$/.test(node.text)) { opaque = true; numericOpaque = true }
    if (ts.isBinaryExpression(node) && node.operatorToken.kind >= ts.SyntaxKind.FirstAssignment && node.operatorToken.kind <= ts.SyntaxKind.LastAssignment) {
      const lhs = node.left
      if (ts.isPropertyAccessExpression(lhs) || ts.isElementAccessExpression(lhs)) {
        const name = member(lhs)
        if (name !== undefined && calledMembers.has(name)) opaque = true
        if (name !== undefined) addProperty(name, node.operatorToken.kind === ts.SyntaxKind.EqualsToken ? node.right : undefined)
        else if (!ts.isElementAccessExpression(lhs) || !lhs.argumentExpression || !numeric(lhs.argumentExpression)) opaque = true
      } else if (ts.isIdentifier(lhs)) variables.set(lhs.text, undefined)
      else opaque = true
    }
    if (ts.isPrefixUnaryExpression(node) || ts.isPostfixUnaryExpression(node) || ts.isDeleteExpression(node)) {
      const operand = ts.isDeleteExpression(node) ? node.expression : node.operand
      if (ts.isDeleteExpression(node) || node.operator === ts.SyntaxKind.PlusPlusToken || node.operator === ts.SyntaxKind.MinusMinusToken) {
        const name = member(operand)
        if (name !== undefined) addProperty(name, undefined)
        else if (ts.isIdentifier(operand)) variables.set(operand.text, undefined)
        else if (!ts.isElementAccessExpression(operand) || !operand.argumentExpression || !numeric(operand.argumentExpression)) opaque = true
      }
    }
    const arrayDeclaration = ts.isIdentifier(node) ? declarationOf(node) : undefined
    if (arrayDeclaration && ts.isIdentifier(arrayDeclaration.name) && arrays.has(arrayDeclaration.name.text)) {
      const arrayName = arrayDeclaration.name.text
      const parent = node.parent
      const declaration = ts.isVariableDeclaration(parent) && parent.name === node
      const imported = ts.isImportSpecifier(parent) || ts.isExportSpecifier(parent)
      const indexed = ts.isElementAccessExpression(parent) && parent.expression === node && parent.argumentExpression && numeric(parent.argumentExpression)
      const operation = parent.parent
      const mutated = indexed && ((ts.isBinaryExpression(operation) && operation.left === parent && operation.operatorToken.kind >= ts.SyntaxKind.FirstAssignment && operation.operatorToken.kind <= ts.SyntaxKind.LastAssignment) || ts.isDeleteExpression(operation) || ts.isPrefixUnaryExpression(operation) || ts.isPostfixUnaryExpression(operation))
      if ((!declaration && !imported && !indexed) || mutated) escapedArrays.add(arrayName)
    }
    ts.forEachChild(node, writes)
  }
  for (const source of sources.values()) writes(source)
  const merge = (values: Array<string[] | undefined>): string[] | undefined => {
    if (values.some(value => value === undefined)) return undefined
    const result = [...new Set(values.flat() as string[])]; return result.length <= 64 ? result : undefined
  }
  const resolve = (node: ts.Expression | undefined, seen = new Set<string>()): string[] | undefined => {
    if (!node || opaque) return undefined
    if (ts.isParenthesizedExpression(node)) return resolve(node.expression, seen)
    if (ts.isStringLiteralLike(node) || ts.isNumericLiteral(node)) return [node.text]
    if (ts.isConditionalExpression(node)) return merge([resolve(node.whenTrue, new Set(seen)), resolve(node.whenFalse, new Set(seen))])
    if (ts.isIdentifier(node)) {
      const declaration = declarationOf(node)
      if (!declaration || !ts.isIdentifier(declaration.name)) return undefined
      const key = 'variable:' + declaration.name.text; if (seen.has(key)) return undefined; seen.add(key)
      return resolve(variables.get(declaration.name.text), seen)
    }
    if (ts.isElementAccessExpression(node) && ts.isIdentifier(node.expression) && node.argumentExpression && numeric(node.argumentExpression)) {
      const declaration = declarationOf(node.expression)
      if (!declaration || !ts.isIdentifier(declaration.name)) return undefined
      const name = declaration.name.text, array = variables.get(name)
      const key = 'array:' + name; if (seen.has(key)) return undefined; seen.add(key)
      if (escapedArrays.has(name) || !array || !ts.isArrayLiteralExpression(array)) return undefined
      return merge(array.elements.map(item => ts.isExpression(item) ? resolve(item, new Set(seen)) : undefined))
    }
    const name = member(node)
    if (name !== undefined) {
      // A coincidentally equal property name on an opaque/any receiver is not
      // a source proof. Require the receiver's declared local field/getter.
      if (!(ts.isPropertyAccessExpression(node) || ts.isElementAccessExpression(node)) || !localReceiver(node.expression)) return undefined
      const at = ts.isPropertyAccessExpression(node) ? node.name : ts.isElementAccessExpression(node) ? node.argumentExpression : node
      const symbol = at && typeChecker().getSymbolAtLocation(at)
      if (!symbol?.declarations?.some(declaration => sources.has(declaration.getSourceFile().fileName) &&
          (ts.isPropertyDeclaration(declaration) || ts.isGetAccessorDeclaration(declaration) || ts.isPropertyAssignment(declaration)))) return undefined
      const key = 'property:' + name; if (seen.has(key)) return undefined; seen.add(key)
      const values = properties.get(name); return values?.length ? merge(values.map(value => resolve(value, new Set(seen)))) : undefined
    }
    return undefined
  }
  // addRendererFeatures has its own AST. Locate the corresponding expression
  // in this graph so type queries use nodes owned by the proof's program.
  const expressions = new Map<string, ts.Expression>()
  const index = (node: ts.Node): void => { if (ts.isExpression(node)) expressions.set(`${node.getSourceFile().fileName}:${node.pos}:${node.end}`, node); ts.forEachChild(node, index) }
  for (const source of sources.values()) index(source)
  const original = (node: ts.Expression): ts.Expression | undefined => expressions.get(`${node.getSourceFile().fileName}:${node.pos}:${node.end}`)
  return Object.assign((node: ts.Expression): string[] | undefined => resolve(original(node)), {
    // String operations do not mutate stylesheets. Use the receiver's type
    // rather than the finite-literal proof: parameters, mutable variables and
    // imported return values can be strings without having known contents.
    // Mixed string/object unions and unresolved any/unknown retain support.
    isString(node: ts.Expression): boolean {
      const at = original(node)
      if (!at || initiallyOpaque) return false
      const type = typeChecker().getTypeAtLocation(at)
      const alternatives = type.isUnion() ? type.types : [type]
      return alternatives.every(value => !!(value.flags & ts.TypeFlags.StringLike))
    },
    // A boolean named animate is an application flag, never a Web Animations
    // API reference. Unknown or mixed types still retain animation support.
    isBoolean(node: ts.Expression): boolean {
      const at = original(node)
      if (!at || initiallyOpaque) return false
      const type = typeChecker().getTypeAtLocation(at)
      const alternatives = type.isUnion() ? type.types : [type]
      return alternatives.every(value => !!(value.flags & ts.TypeFlags.BooleanLike))
    },
    // In a statically lowered numeric slot no value can contain a CSS '%'.
    // Ambiguous imports, assertions and any/unknown retain storage. A typed
    // native number remains numeric when unrelated literal objects escape;
    // unlike the colour proof, its exact value need not be known.
    isNumeric(node: ts.Expression): boolean {
      const at = original(node)
      return !!at && !numericOpaque && numeric(at)
    },
    // A user-authored method called animate is not Element.animate. Require
    // both its implementation in the scanned graph and a local receiver;
    // an opaque/DOM receiver merely cast to a class is not sufficient.
    isSourceMethod(node: ts.Expression): boolean {
      const at = original(node)
      if (!at || initiallyOpaque) return false
      const parent = at.parent
      // A locally declared frame callback may also be named animate. Resolve
      // its binding instead of confusing the name with Element.animate.
      if (ts.isIdentifier(at)) {
        let symbol = typeChecker().getSymbolAtLocation(at)
        if (symbol && (symbol.flags & ts.SymbolFlags.Alias)) symbol = typeChecker().getAliasedSymbol(symbol)
        const declarations = symbol?.declarations
        if (declarations?.length && declarations.every(declaration =>
          sources.has(declaration.getSourceFile().fileName) && ts.isFunctionDeclaration(declaration) && !!declaration.body)) {
          let reassigned = false
          const checkWrites = (child: ts.Node): void => {
            if (ts.isBinaryExpression(child) && child.operatorToken.kind >= ts.SyntaxKind.FirstAssignment &&
                child.operatorToken.kind <= ts.SyntaxKind.LastAssignment) {
              const checkTarget = (target: ts.Node): void => {
                if (ts.isIdentifier(target) && typeChecker().getSymbolAtLocation(target) === symbol) reassigned = true
                ts.forEachChild(target, checkTarget)
              }
              checkTarget(child.left)
            }
            ts.forEachChild(child, checkWrites)
          }
          for (const source of sources.values()) checkWrites(source)
          if (!reassigned) return true
        }
      }
      if (ts.isMethodDeclaration(parent) && parent.name === at && parent.body) return true
      const access = ts.isPropertyAccessExpression(at) || ts.isElementAccessExpression(at) ? at
        : ts.isPropertyAccessExpression(parent) && parent.name === at ? parent : undefined
      if (!access || !localReceiver(access.expression)) return false
      const name = ts.isPropertyAccessExpression(access) ? access.name : access.argumentExpression
      const declarations = name && typeChecker().getSymbolAtLocation(name)?.declarations
      return !!declarations?.length && declarations.every(declaration =>
        sources.has(declaration.getSourceFile().fileName) && ts.isMethodDeclaration(declaration) && !!declaration.body)
    },
  })
}
