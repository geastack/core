import ts from 'typescript'

export const nodeAuxVersion = 'node-aux-v1'
export const nodeAuxFeatures = ['node-listeners', 'node-attributes', 'node-default-styles'] as const
export function addUnknownNodeAux(features: Set<string>): void {
  for (const feature of nodeAuxFeatures) features.add(feature)
}

// The CSS observer supplies a whole stylesheet. Only rule preludes may
// select attributes: declarations such as color:#fff do not need an ID store.
export function cssUsesNodeAttributes(css: string): boolean {
  let prelude = '', quote = '', escaped = false
  for (const ch of css) {
    if (quote) {
      prelude += ch
      if (escaped) escaped = false
      else if (ch === '\\') escaped = true
      else if (ch === quote) quote = ''
      continue
    }
    if (ch === '"' || ch === "'") { quote = ch; prelude += ch; continue }
    if (ch === '{') {
      if (/[#\[]/.test(prelude)) return true
      prelude = ''
    } else if (ch === '}' || ch === ';') prelude = ''
    else prelude += ch
  }
  return !!quote
}

// This proof covers authored JSX and all discovered source, not a runtime
// census. Unresolved imports/native code, opaque DOM calls, spreads and native
// controls preserve the full owners. Ordinary class/style bindings use their
// own stores and do not require a generic attribute list.
export function addNodeAuxFeatures(file: string, text: string, features: Set<string>, isNumeric: (node: ts.Expression) => boolean): void {
  if (/\.css$/i.test(file)) return
  const source = ts.createSourceFile(file, text, ts.ScriptTarget.Latest, true, /\.[jt]sx$/i.test(file) ? ts.ScriptKind.TSX : ts.ScriptKind.TS)
  const unknown = (): void => addUnknownNodeAux(features)
  const token = (name: string): void => {
    if (/^(?:addEventListener|removeEventListener|setEventListener|dispatchEvent|on[A-Z].*|on(?:click|pointer.*|touch.*|input|keydown|scroll)|click|touchstart|touchmove|touchend|pointerdown|pointermove|pointerup|input|keydown|scroll)$/.test(name)) features.add('node-listeners')
    if (/^(?:id|dataset|attributes|getElementById|querySelector|querySelectorAll|ensureAppRoot|setAttribute|getAttribute|removeAttribute|hasAttribute|toggleAttribute|setPressId|setPressValue|pressId|pressValue|data-press-id|data-press-value)$/.test(name)) features.add('node-attributes')
    if (name === 'setDefaultStyle') features.add('node-default-styles')
    if (/^(?:eval|Function|Reflect|Proxy|Object|DOMParser|innerHTML|outerHTML|insertAdjacentHTML|createElement|createElementNS|parseFromString|createContextualFragment|__gea_.*)$/.test(name) && name !== '__gea_Display') unknown()
  }
  const visit = (node: ts.Node): void => {
    if (ts.isIdentifier(node) || ts.isStringLiteralLike(node)) token(node.text)
    if (ts.isJsxSpreadAttribute(node)) unknown()
    if (ts.isJsxAttribute(node)) {
      const name = node.name.getText(source)
      if (!/^(?:class|className|style|children|key)$/.test(name)) features.add('node-attributes')
      if (/^on/i.test(name)) features.add('node-listeners')
      if (name === 'ref') unknown()
      token(name)
    }
    if (ts.isJsxOpeningElement(node) || ts.isJsxSelfClosingElement(node)) {
      const tag = node.tagName.getText(source)
      // Other builtins can install native listeners, attributes or defaults.
      if (/^[a-z]/.test(tag) && !/^(?:div|span|p|section|main|header|footer|article|aside|nav)$/.test(tag)) unknown()
    }
    if (ts.isElementAccessExpression(node) && node.argumentExpression &&
        !ts.isStringLiteralLike(node.argumentExpression) && !isNumeric(node.argumentExpression)) unknown()
    if (ts.isImportDeclaration(node) && ts.isStringLiteral(node.moduleSpecifier) &&
        /^(?:gea-embedded|@geastack\/(?:core|engine)|@geajs\/core)(?:\/|$)/.test(node.moduleSpecifier.text) &&
        node.importClause && !node.importClause.isTypeOnly) {
      const bindings = node.importClause.namedBindings
      if (node.importClause.name || !bindings || !ts.isNamedImports(bindings) ||
          bindings.elements.some(item => !item.isTypeOnly && !/^(?:Component|Store|mount|Display)$/.test((item.propertyName ?? item.name).text))) unknown()
    }
    ts.forEachChild(node, visit)
  }
  visit(source)
}
