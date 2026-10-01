import ts from 'typescript'

export const nodeAnalysisVersion = 'node-analysis-v1'
export const nodeFeatures = ['node-images', 'node-inputs'] as const
export function addUnknownNodeFeatures(features: Set<string>): void {
  for (const feature of nodeFeatures) features.add(feature)
}

// Native node payloads are selected from the whole discovered source graph.
// CSS rendering features remain independent: a background image does not make
// an <img> node, and ordinary buttons do not need a native text-input keyboard.
export function addNodeFeatures(file: string, text: string, features: Set<string>): void {
  if (/\.css$/i.test(file)) return
  const source = ts.createSourceFile(file, text, ts.ScriptTarget.Latest, true, /\.[jt]sx$/i.test(file) ? ts.ScriptKind.TSX : ts.ScriptKind.TS)
  const unknown = (): void => addUnknownNodeFeatures(features)
  const name = (value: string): void => {
    if (/^(?:img|image)$/i.test(value) || /^(?:ImageElement|HTMLImageElement|createImage|loadImage)$/.test(value)) features.add('node-images')
    if (/^(?:input|textarea|select|contenteditable)$/i.test(value) || /^(?:TextInput|InputElement|TextInputElement|HTMLInputElement|HTMLTextAreaElement|HTMLSelectElement|VirtualKeyboard|createInput|createTextInput)$/.test(value)) features.add('node-inputs')
  }
  const visit = (node: ts.Node): void => {
    if (ts.isIdentifier(node)) {
      name(node.text)
      // A destructured or bare factory can receive a tag outside this call site.
      if (/^(?:createElement|createElementNS)$/.test(node.text) &&
          !(ts.isPropertyAccessExpression(node.parent) && node.parent.name === node)) unknown()
      if (/^__gea_/.test(node.text) && node.text !== '__gea_Display') unknown()
    }
    if (ts.isStringLiteralLike(node)) {
      name(node.text)
      if (/^(?:createElement|createElementNS)$/.test(node.text) &&
          !ts.isElementAccessExpression(node.parent)) unknown()
      if (/<(?:img|image|input|textarea|select)\b/i.test(node.text)) unknown()
    }
    if (ts.isJsxOpeningElement(node) || ts.isJsxSelfClosingElement(node)) name(node.tagName.getText(source).split(':').at(-1)!)
    // Display controls the panel/canvas; importing it cannot create native
    // image or input nodes. Calls that actually create nodes are still scanned.
    if (ts.isImportDeclaration(node) && ts.isStringLiteral(node.moduleSpecifier) &&
        /^(?:gea-embedded|@geastack\/(?:core|engine)|@geajs\/core)(?:\/|$)/.test(node.moduleSpecifier.text) &&
        node.importClause && !node.importClause.isTypeOnly) {
      const bindings = node.importClause.namedBindings
      if (node.importClause.name || !bindings || !ts.isNamedImports(bindings) ||
          bindings.elements.some(item => !item.isTypeOnly && !/^(?:Component|Store|mount|Display)$/.test((item.propertyName ?? item.name).text))) unknown()
    }
    // Mutation/markup helpers can create descendants whose tags are not present
    // as JSX. Known literal factories are covered by their tag strings above.
    if (ts.isPropertyAccessExpression(node) || ts.isElementAccessExpression(node)) {
      const member = ts.isPropertyAccessExpression(node) ? node.name.text : node.argumentExpression && ts.isStringLiteralLike(node.argumentExpression) ? node.argumentExpression.text : undefined
      if (member && /^(?:innerHTML|outerHTML|insertAdjacentHTML|parseFromString|createContextualFragment|write|writeln|tagName|setTagName)$/.test(member)) unknown()
      if (member && /^(?:createElement|createElementNS)$/.test(member)) {
        const call = node.parent
        const argument = ts.isCallExpression(call) && call.expression === node ? call.arguments[member === 'createElementNS' ? 1 : 0] : undefined
        if (!argument || !ts.isStringLiteralLike(argument)) unknown()
      }
    }
    if (ts.isIdentifier(node) && /^(?:DOMParser|eval|Function)$/.test(node.text)) unknown()
    ts.forEachChild(node, visit)
  }
  visit(source)
}
