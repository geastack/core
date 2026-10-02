import ts from 'typescript'

export interface WorkerModuleAnalysis {
  realms: boolean
  modules: string[]
  unknown: boolean
}

/** Standard module-worker/worklet entry points are executable dependencies. */
export function analyzeWorkerModules(file: string, text: string): WorkerModuleAnalysis {
  const source = ts.createSourceFile(file, text, ts.ScriptTarget.Latest, true,
    /\.[jt]sx$/i.test(file) ? ts.ScriptKind.TSX : ts.ScriptKind.TS)
  const result: WorkerModuleAnalysis = { realms: false, modules: [], unknown: false }
  const scopes = lexicalBindings(source)
  const name = (expression: ts.Expression): string | undefined => {
    if (ts.isIdentifier(expression)) return scopes.shadows(expression, expression.text) ? undefined : expression.text
    if (ts.isPropertyAccessExpression(expression) && ts.isIdentifier(expression.expression) &&
      ['globalThis', 'window', 'self'].includes(expression.expression.text) &&
      !scopes.shadows(expression, expression.expression.text)) return expression.name.text
    return undefined
  }
  const module = (expression: ts.Expression | undefined): void => {
    result.realms = true
    if (expression && ts.isNewExpression(expression) && name(expression.expression) === 'URL') {
      const [path, base] = expression.arguments ?? []
      if (path && ts.isStringLiteralLike(path) && base && ts.isPropertyAccessExpression(base) &&
        base.name.text === 'url' && ts.isMetaProperty(base.expression) && base.expression.keywordToken === ts.SyntaxKind.ImportKeyword) {
        result.modules.push(path.text)
        return
      }
    }
    if (expression && ts.isStringLiteralLike(expression)) result.modules.push(expression.text)
    else result.unknown = true
  }
  const visit = (node: ts.Node): void => {
    if (ts.isNewExpression(node)) {
      const constructor = name(node.expression)
      if (constructor === 'Worker') module(node.arguments?.[0])
      if (constructor === 'AudioWorkletNode') result.realms = true
    }
    if (ts.isCallExpression(node)) {
      if (name(node.expression) === 'registerProcessor') result.realms = true
      if (ts.isPropertyAccessExpression(node.expression) && node.expression.name.text === 'addModule') {
        const receiver = node.expression.expression
        if (ts.isPropertyAccessExpression(receiver) && receiver.name.text === 'audioWorklet') module(node.arguments[0])
      }
    }
    ts.forEachChild(node, visit)
  }
  visit(source)
  return result
}


interface Scope {
  parent?: Scope
  bindings: Set<string>
  function: boolean
}

/** Value bindings shadow browser globals; erased type imports never do. */
function lexicalBindings(source: ts.SourceFile): { shadows(node: ts.Node, name: string): boolean } {
  const owners = new Map<ts.Node, Scope>()
  const bind = (scope: Scope, name: ts.BindingName): void => {
    if (ts.isIdentifier(name)) scope.bindings.add(name.text)
    else for (const element of name.elements) if (ts.isBindingElement(element)) bind(scope, element.name)
  }
  const walk = (node: ts.Node, inherited: Scope): void => {
    let scope = inherited
    if (ts.isSourceFile(node) || ts.isBlock(node) || ts.isFunctionLike(node) || ts.isCatchClause(node)) {
      scope = { parent: inherited, bindings: new Set(), function: ts.isSourceFile(node) || ts.isFunctionLike(node) }
    }
    owners.set(node, scope)
    if (ts.isImportDeclaration(node) && node.importClause && !node.importClause.isTypeOnly) {
      const clause = node.importClause
      if (clause.name) bind(scope, clause.name)
      if (clause.namedBindings) {
        if (ts.isNamespaceImport(clause.namedBindings)) bind(scope, clause.namedBindings.name)
        else for (const item of clause.namedBindings.elements) if (!item.isTypeOnly) bind(scope, item.name)
      }
    }
    if (ts.isImportEqualsDeclaration(node) && !node.isTypeOnly) bind(scope, node.name)
    if ((ts.isFunctionDeclaration(node) || ts.isClassDeclaration(node) || ts.isEnumDeclaration(node)) && node.name) {
      bind(ts.isFunctionDeclaration(node) ? inherited : scope, node.name)
    }
    if (ts.isFunctionExpression(node) && node.name) bind(scope, node.name)
    if (ts.isParameter(node)) bind(scope, node.name)
    if (ts.isVariableDeclaration(node)) {
      let target = scope
      const list = node.parent
      if (ts.isVariableDeclarationList(list) && !(list.flags & ts.NodeFlags.BlockScoped)) {
        while (!target.function && target.parent) target = target.parent
      }
      bind(target, node.name)
    }
    ts.forEachChild(node, (child) => walk(child, scope))
  }
  walk(source, { bindings: new Set(), function: true })
  return {
    shadows(node, name) {
      for (let scope = owners.get(node); scope; scope = scope.parent) if (scope.bindings.has(name)) return true
      return false
    },
  }
}
