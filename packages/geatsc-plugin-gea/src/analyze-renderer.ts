import ts from 'typescript'

export const rendererAnalysisVersion = 'renderer-analysis-v1'
export const rendererOcclusionAnalysisVersion = 'renderer-occlusion-v1'
const rendererFeatures = ['renderer-circles', 'renderer-transforms', 'renderer-linear-gradients', 'renderer-radial-gradients']

export function addUnknownRendererFeatures(features: Set<string>): void {
  for (const feature of rendererFeatures) features.add(feature)
  features.add('renderer-occlusion-triangles')
}

// A cache is an optimization, never a requirement for correct rendering. Source
// analysis selects storage for reachable instructions. Unknown styles preserve
// all relevant optimizations; literal colors/sizes and video canvas operations
// do not reserve shape, transform or gradient tables.
export interface StyleUsageObserver {
  isSourceMethod?(node: ts.Expression): boolean
  isString?(node: ts.Expression): boolean
  isBoolean?(node: ts.Expression): boolean
  selector?(css: string): void
  property(name: string | undefined, value: string | undefined, expression?: ts.Expression): void
  unknown(): void
  unknownRanges?(): void
  unknownCircleBounds?(): void
}

// Proof shared by CSS storage and renderer cache selection. Custom-property
// identifiers are case-sensitive; missing/aliased/opaque definitions retain
// support. The atom-like proof is intentionally limited to literal colors.
export function colorVariableAnalysis() {
  const colors = new Map<string, boolean>()
  let opaque = false
  const color = (value: string | undefined): boolean =>
    value !== undefined && /^(?:#(?:[\da-f]{3}|[\da-f]{4}|[\da-f]{6}|[\da-f]{8})|transparent|currentcolor)$/i.test(value.trim().replace(/\s*!important\s*$/i, ''))
  return {
    property(name: string | undefined, value: string | undefined): void {
      if (!name) { opaque = true; return }
      if (name.startsWith('--')) colors.set(name, (colors.get(name) ?? true) && color(value))
    },
    unknown(): void { opaque = true },
    isColor(value: string | undefined): boolean {
      if (color(value)) return true
      const match = value?.trim().replace(/\s*!important\s*$/i, '').match(/^var\(\s*(--[\w-]+)\s*(?:,\s*([^()]+))?\)$/i)
      return !opaque && !!match && colors.get(match[1]) === true && (match[2] === undefined || color(match[2]))
    },
  }
}

export function rendererVariableAnalysis(features: Set<string>) {
  const colors = colorVariableAnalysis()
  const backgrounds: string[] = []
  return {
    property: colors.property,
    unknown: colors.unknown,
    background(value: string): void { backgrounds.push(value) },
    finish(): void {
      if (backgrounds.some(value => !colors.isColor(value))) {
        features.add('renderer-linear-gradients')
        features.add('renderer-radial-gradients')
      }
    },
  }
}

type RendererVariableAnalysis = ReturnType<typeof rendererVariableAnalysis>

export function addRendererFeatures(file: string, text: string, features: Set<string>, observer?: StyleUsageObserver, variables?: RendererVariableAnalysis): void {
  const allStyles = (): void => {
    observer?.unknown()
    for (const feature of rendererFeatures.slice(1)) features.add(feature)
  }
  const gradients = (value: string, unknown = false): void => {
    if (unknown || /(?:^|[^\w-])(?:repeating-)?linear-gradient\s*\(/i.test(value)) features.add('renderer-linear-gradients')
    if (unknown || /(?:^|[^\w-])(?:repeating-)?radial-gradient\s*\(/i.test(value)) features.add('renderer-radial-gradients')
  }
  const property = (name: string | undefined, value: string | undefined, expression?: ts.Expression): void => {
    observer?.property(name, value, expression)
    if (!name) { allStyles(); return }
    const cssName = name.replace(/[A-Z]/g, (letter) => `-${letter.toLowerCase()}`).toLowerCase()
    // CSS rounded boxes also use the circle span caches in Canvas.
    if (/^border(?:-(?:top-left|top-right|bottom-left|bottom-right))?-radius$/.test(cssName) &&
        (value === undefined || !/^0(?:px)?(?:\s+0(?:px)?)*$/i.test(value.trim()))) features.add('renderer-circles')
    if (/^(?:-webkit-)?(?:transform|perspective|translate|rotate|scale)$/.test(cssName)) {
      if (value?.trim().toLowerCase() !== 'none') features.add('renderer-transforms')
    }
    if (/^(?:background(?:-image)?|border-image(?:-source)?|mask(?:-image)?)$/.test(cssName)) {
      if (value !== undefined && /var\s*\(/i.test(value) && variables) variables.background(value)
      else gradients(value ?? '', value === undefined || /var\s*\(/i.test(value))
    }
    // Custom properties can later supply a background or transform value.
    if (cssName.startsWith('--') && value !== undefined) gradients(value)
  }
  const css = (value: string): void => {
    value = value.replace(/\/\*[\s\S]*?\*\//g, '')
    // CSS escapes can occur in property names and values as well as glyph
    // content. Decode them so an icon escape doesn't force every CSS family.
    value = value.replace(/\\(?:([0-9a-f]{1,6})\s?|([^\r\n]))/gi, (_match, hex: string | undefined, character: string | undefined) => {
      if (!hex) return character ?? ''
      const code = Number.parseInt(hex, 16)
      return String.fromCodePoint(code > 0 && code <= 0x10ffff ? code : 0xfffd)
    })
    observer?.selector?.(value)
    gradients(value)
    for (const match of value.matchAll(/(?:^|[;{])\s*([\w-]+)\s*:\s*([^;{}]*)/g)) property(match[1].startsWith('--') ? match[1] : match[1].toLowerCase(), match[2])
  }
  if (/\.css$/i.test(file)) { css(text); return }

  const source = ts.createSourceFile(file, text, ts.ScriptTarget.Latest, true, /\.[jt]sx$/i.test(file) ? ts.ScriptKind.TSX : ts.ScriptKind.TS)
  const constants = new Map<string, ts.Expression>()
  const ambiguousConstants = new Set<string>()
  const bindingCounts = new Map<string, number>()
  const bind = (name: ts.BindingName): void => {
    if (ts.isIdentifier(name)) {
      const count = (bindingCounts.get(name.text) ?? 0) + 1
      bindingCounts.set(name.text, count)
      if (count > 1) ambiguousConstants.add(name.text)
    } else for (const part of name.elements) if (ts.isBindingElement(part)) bind(part.name)
  }
  const collect = (node: ts.Node): void => {
    if (ts.isVariableDeclaration(node) || ts.isParameter(node)) bind(node.name)
    else if ((ts.isImportClause(node) || ts.isImportSpecifier(node) || ts.isNamespaceImport(node) ||
              ts.isFunctionDeclaration(node) || ts.isClassDeclaration(node)) && node.name) bind(node.name)
    if (ts.isVariableDeclaration(node) && ts.isIdentifier(node.name) && node.initializer &&
        ts.isVariableDeclarationList(node.parent) && (node.parent.flags & ts.NodeFlags.Const)) {
      constants.set(node.name.text, node.initializer)
    }
    ts.forEachChild(node, collect)
  }
  collect(source)
  const unwrap = (node: ts.Expression): ts.Expression => {
    while (ts.isParenthesizedExpression(node) || ts.isAsExpression(node) || ts.isTypeAssertionExpression(node) || ts.isSatisfiesExpression(node)) node = node.expression
    return node
  }
  const literal = (node: ts.Expression | undefined, seen = new Set<string>()): string | undefined => {
    if (!node) return undefined
    node = unwrap(node)
    if (ts.isStringLiteralLike(node) || ts.isNumericLiteral(node)) return node.text
    if (ts.isIdentifier(node) && !ambiguousConstants.has(node.text) && !seen.has(node.text)) {
      seen.add(node.text)
      return literal(constants.get(node.text), seen)
    }
    if (ts.isBinaryExpression(node) && node.operatorToken.kind === ts.SyntaxKind.PlusToken) {
      const left = literal(node.left, new Set(seen)), right = literal(node.right, new Set(seen))
      if (left !== undefined && right !== undefined) return left + right
    }
    return undefined
  }
  const member = (node: ts.Node): string | undefined => {
    if (ts.isPropertyAccessExpression(node)) return node.name.text
    if (ts.isElementAccessExpression(node)) return literal(node.argumentExpression)
    return undefined
  }
  const isStyleTarget = (node: ts.Expression, seen = new Set<string>()): boolean => {
    node = unwrap(node)
    if (member(node) === 'style') return true
    if (ts.isIdentifier(node) && constants.has(node.text) && !ambiguousConstants.has(node.text) && !seen.has(node.text)) {
      seen.add(node.text)
      return isStyleTarget(constants.get(node.text)!, seen)
    }
    return false
  }
  const styleObjectBindings = new Set<string>()
  const styleObject = (expression: ts.Expression, seen = new Set<string>()): void => {
    expression = unwrap(expression)
    if (ts.isIdentifier(expression)) styleObjectBindings.add(expression.text)
    if (ts.isIdentifier(expression) && constants.has(expression.text) && !ambiguousConstants.has(expression.text) && !seen.has(expression.text)) {
      seen.add(expression.text)
      styleObject(constants.get(expression.text)!, seen)
    } else if (ts.isObjectLiteralExpression(expression)) {
      for (const item of expression.properties) {
        if (ts.isSpreadAssignment(item)) { styleObject(item.expression, new Set(seen)); continue }
        if (!ts.isPropertyAssignment(item) && !ts.isShorthandPropertyAssignment(item)) { allStyles(); continue }
        const name = ts.isComputedPropertyName(item.name) ? literal(item.name.expression) : item.name.text
        property(name, literal(ts.isPropertyAssignment(item) ? item.initializer : item.name), ts.isPropertyAssignment(item) ? item.initializer : item.name)
      }
    } else if (ts.isConditionalExpression(expression)) {
      styleObject(expression.whenTrue, new Set(seen)); styleObject(expression.whenFalse, new Set(seen))
    } else {
      const value = literal(expression)
      if (value === undefined) allStyles()
      else css(value)
    }
  }
  const isStyleObjectReference = (expression: ts.Expression, seen = new Set<string>()): boolean => {
    expression = unwrap(expression)
    if (!ts.isIdentifier(expression) || seen.has(expression.text)) return false
    if (styleObjectBindings.has(expression.text)) return true
    seen.add(expression.text)
    const initializer = constants.get(expression.text)
    return !!initializer && isStyleObjectReference(initializer, seen)
  }
  const argumentContainsStyle = (expression: ts.Node): boolean => {
    if (ts.isExpression(expression) && isStyleObjectReference(expression)) return true
    let found = false
    ts.forEachChild(expression, child => { if (argumentContainsStyle(child)) found = true })
    return found
  }
  const opaqueCallable = (node: ts.Expression, seen = new Set<string>()): boolean => {
    node = unwrap(node)
    if (ts.isIdentifier(node) && !seen.has(node.text) && constants.has(node.text)) {
      seen.add(node.text)
      return opaqueCallable(constants.get(node.text)!, seen)
    }
    if (ts.isElementAccessExpression(node) && member(node) === undefined) return true
    if (ts.isPropertyAccessExpression(node) || ts.isElementAccessExpression(node)) return opaqueCallable(node.expression, seen)
    return false
  }
  const circleMethods = new Set(['arc', 'arcTo', 'ellipse', 'roundRect', 'fillCircle', 'fillCircleRgb565', 'drawImageCircle', 'drawImageRounded', 'fillCircles', 'fillCirclesRgb565', 'fillCirclesRgb565Uniform', 'fillCirclesRgb565WorldYSorted', 'fillCircleBox', 'fillEllipse', 'fillEllipseBatch', 'fillRoundedRectBoxesRgb565', 'fillRoundedRect', 'strokeCircle', 'strokeRoundedRect'])
  const visit = (node: ts.Node): void => {
    // Include references so aliased/destructured drawing cannot evade the proof.
    if ((ts.isIdentifier(node) || ts.isStringLiteralLike(node)) && /^(?:fillTriangle\w*|TriangleEntry|eval|Function|Reflect)$/.test(node.text)) features.add('renderer-occlusion-triangles')
    if (ts.isIdentifier(node) && /^__gea_/.test(node.text) && node.text !== '__gea_Display') features.add('renderer-occlusion-triangles')
    // CSS radius bounds say nothing about imperative drawing, including aliases
    // and destructured/bare methods. Keep their full caches conservatively.
    if ((ts.isIdentifier(node) || ts.isStringLiteralLike(node)) && circleMethods.has(node.text)) observer?.unknownCircleBounds?.()
    if (ts.isIdentifier(node) && /^__gea_/.test(node.text) && node.text !== '__gea_Display') observer?.unknownRanges?.()
    // These factories/hosts can inject native defaults or change length scaling
    // outside the authored CSS range proof. Type-only imports cannot do so.
    // Display alone is safe: actual DPR setters and opaque calls below still
    // invalidate bounds, including aliased and bracket-access setters.
    if (ts.isImportDeclaration(node) && ts.isStringLiteral(node.moduleSpecifier) &&
        /^(?:gea-embedded|@geastack\/(?:core|engine)|@geajs\/core)(?:\/|$)/.test(node.moduleSpecifier.text) &&
        node.importClause && !node.importClause.isTypeOnly) {
      const bindings = node.importClause.namedBindings
      if (node.importClause.name || !bindings || !ts.isNamedImports(bindings) ||
          bindings.elements.some(item => !item.isTypeOnly && !/^(?:Component|Store|mount|Display)$/.test((item.propertyName ?? item.name).text))) { observer?.unknownRanges?.(); features.add('renderer-occlusion-triangles') }
    }
    if (ts.isIdentifier(node) && /^(?:setDevicePixelRatio|setViewportMetrics|devicePixelRatio|__gea_StyleSheet|createVirtualList|createInput|createTextInput|VirtualList|VirtualListElement|TextInput|VirtualKeyboard)$/.test(node.text)) observer?.unknownRanges?.()
    if ((ts.isStringLiteralLike(node) && /^(?:virtual-list|input|textarea|select)$/i.test(node.text)) ||
        ((ts.isJsxOpeningElement(node) || ts.isJsxSelfClosingElement(node)) && /^(?:virtual-list|input|textarea|select)$/i.test(node.tagName.getText(source)))) observer?.unknownRanges?.()
    // Preserve intrinsic text defaults even when no CSS declaration spells
    // them out. These observations also cover literal createElement tags.
    const intrinsicTag = ts.isStringLiteralLike(node) ? node.text.toLowerCase()
      : ts.isJsxOpeningElement(node) || ts.isJsxSelfClosingElement(node) ? node.tagName.getText(source).toLowerCase() : ''
    if (/^(?:button|img|image|canvas|video|camera|audio|virtual-list|input|textarea|select)$/.test(intrinsicTag)) observer?.unknownCircleBounds?.()
    if (/^(?:b|strong|h[1-6]|th)$/.test(intrinsicTag)) observer?.property('font-weight', undefined)
    if (/^(?:pre|textarea)$/.test(intrinsicTag)) observer?.property('white-space', undefined)
    if (/^(?:center|th)$/.test(intrinsicTag)) observer?.property('text-align', undefined)
    // Native controls can introduce scrollable descendants without authored
    // overflow CSS. Literal tag/factory use retains state; dynamic factories
    // and computed calls are opaque and keep all families below.
    if ((ts.isIdentifier(node) && /^(?:createVirtualList|createInput|createTextInput|VirtualList|VirtualListElement|TextInput|VirtualKeyboard)$/.test(node.text)) ||
        (ts.isStringLiteralLike(node) && /^(?:virtual-list|input|textarea|select)$/i.test(node.text)) ||
        ((ts.isJsxOpeningElement(node) || ts.isJsxSelfClosingElement(node)) && /^(?:virtual-list|input|textarea|select)$/i.test(node.tagName.getText(source)))) { features.add('css-scrolling'); features.add('css-overflow-axes'); observer?.property('inset', undefined) }
    // Imperative animation APIs can mutate arbitrary style fields. Observe
    // references as well as calls so aliases cannot bypass the proof. Frame
    // scheduling (requestAnimationFrame) is independent and stays available.
    if (ts.isIdentifier(node) && /^(?:animate|KeyframeEffect|Animation|AnimationEngine|DeclarativeAnimations|Reflect)$/.test(node.text) &&
        !(node.text === 'animate' && (observer?.isSourceMethod?.(node) || observer?.isBoolean?.(node)))) allStyles()
    if (ts.isBindingElement(node) && /^(?:createElement|createElementNS|animate|setAttribute|insertRule|replaceSync)$/.test(node.propertyName?.getText(source).replace(/^['"]|['"]$/g, '') ?? node.name.getText(source))) allStyles()
    if (ts.isBindingElement(node) && node.propertyName && ts.isComputedPropertyName(node.propertyName) && literal(node.propertyName.expression) === undefined) allStyles()
    if (ts.isBindingElement(node) && node.propertyName && ts.isComputedPropertyName(node.propertyName) && /^fillTriangle\w*$/.test(literal(node.propertyName.expression) ?? '')) features.add('renderer-occlusion-triangles')
    if (ts.isTaggedTemplateExpression(node) && ts.isTemplateExpression(node.template)) allStyles()
    // Literal CSS also covers CSS templates, DOM setters and imported helpers.
    if (ts.isStringLiteralLike(node)) css(node.text)
    if (ts.isPropertyAccessExpression(node) || ts.isElementAccessExpression(node)) {
      const name = member(node)
      if (name && /^(?:setDevicePixelRatio|setViewportMetrics|devicePixelRatio)$/.test(name)) observer?.unknownRanges?.()
      if (name && circleMethods.has(name)) { features.add('renderer-circles'); observer?.unknownCircleBounds?.() }
      if (name && /^fillTriangle\w*$/.test(name)) features.add('renderer-occlusion-triangles')
      if (name === 'dataset' || (name === 'animate' && !observer?.isSourceMethod?.(node))) allStyles()
      if (name && /^(?:setAttribute|insertRule|replaceSync)$/.test(name) && !(ts.isCallExpression(node.parent) && node.parent.expression === node)) allStyles()
      if (name && /^(?:getOwnPropertyDescriptor|getOwnPropertyDescriptors|getPrototypeOf|setPrototypeOf|defineProperty|defineProperties)$/.test(name)) allStyles()
      if ((name === 'createElement' || name === 'createElementNS') && !(ts.isCallExpression(node.parent) && node.parent.expression === node)) allStyles()
      if (name === 'style') {
        const parent = node.parent
        const knownMember = (ts.isPropertyAccessExpression(parent) || ts.isElementAccessExpression(parent)) && parent.expression === node
        const knownAssignment = ts.isBinaryExpression(parent) && parent.left === node
        if (!knownMember && !knownAssignment) observer?.unknown()
      }
      if (name === 'setProperty' && !(ts.isCallExpression(node.parent) && node.parent.expression === node)) observer?.unknown()
    }
    // Native declarative animations can write style fields through data-anim.
    if (ts.isJsxAttribute(node) && node.name.getText(source) === 'data-anim') allStyles()
    if (ts.isJsxAttribute(node) && node.name.getText(source) === 'dir') observer?.property('direction', undefined)
    if (ts.isJsxAttribute(node) && node.name.getText(source) === 'style' && node.initializer) {
      if (ts.isJsxExpression(node.initializer) && node.initializer.expression) styleObject(node.initializer.expression)
    }
    if (ts.isJsxSpreadAttribute(node)) {
      const value = unwrap(node.expression)
      if (ts.isObjectLiteralExpression(value)) {
        for (const item of value.properties) if (ts.isPropertyAssignment(item) && item.name.getText(source) === 'style') styleObject(item.initializer)
      } else allStyles()
    }
    // Property names are relevant even for aliases of el.style. This deliberately
    // errs toward keeping a cache when an unrelated object has a style key.
    if (ts.isPropertyAssignment(node)) {
      const name = ts.isComputedPropertyName(node.name) ? literal(node.name.expression) : node.name.getText(source).replace(/^['"]|['"]$/g, '')
      if (name === 'data-anim') allStyles()
      if (name !== undefined) property(name, literal(node.initializer), node.initializer)
      if (name === 'style') styleObject(node.initializer)
    }
    if (ts.isBinaryExpression(node) && node.operatorToken.kind === ts.SyntaxKind.EqualsToken) {
      const name = member(node.left)
      if (name === 'dir') observer?.property('direction', undefined)
      if (name === 'innerHTML' || name === 'outerHTML' || name === 'adoptedStyleSheets') allStyles()
      if (name === 'style' || name === 'cssText') styleObject(node.right)
      else if (ts.isPropertyAccessExpression(node.left) || ts.isElementAccessExpression(node.left)) {
        if (name !== undefined || isStyleTarget(node.left.expression)) property(name, literal(node.right), node.right)
        else if (isStyleObjectReference(node.left.expression)) observer?.unknown()
      }
    }
    if (ts.isCallExpression(node)) {
      if (opaqueCallable(node.expression)) allStyles()
      // A helper receiving a style object can mutate it through aliases,
      // Object.assign or computed keys that this source scan cannot resolve.
      if (node.arguments.some(argumentContainsStyle)) observer?.unknown()
      const name = member(node.expression)
      if ((ts.isPropertyAccessExpression(node.expression) || ts.isElementAccessExpression(node.expression)) && isStyleTarget(node.expression.expression) && name !== 'setProperty' && name !== 'removeProperty') observer?.unknownRanges?.()
      if (node.expression.kind === ts.SyntaxKind.ImportKeyword && literal(node.arguments[0]) === undefined) { addUnknownRendererFeatures(features); observer?.unknown() }
      if (name && circleMethods.has(name)) { features.add('renderer-circles'); observer?.unknownCircleBounds?.() }
      if (name && /^fillTriangle\w*$/.test(name)) features.add('renderer-occlusion-triangles')
      if (name === 'createLinearGradient') features.add('renderer-linear-gradients')
      if (name === 'createRadialGradient') features.add('renderer-radial-gradients')
      if (name === 'registerProperty') allStyles()
      if ((name === 'createElement' || name === 'createElementNS') && literal(node.arguments[name === 'createElementNS' ? 1 : 0]) === undefined) allStyles()
      if (name === 'createElement' && literal(node.arguments[0]) === 'style') allStyles()
      if (name === 'setAttribute' && literal(node.arguments[0]) === 'dir') observer?.property('direction', undefined)
      if (name === 'setProperty') property(literal(node.arguments[0]), literal(node.arguments[1]), node.arguments[1])
      if (name === 'setAttribute' && (!literal(node.arguments[0]) || literal(node.arguments[0]) === 'data-anim')) allStyles()
      if (name === 'setAttribute' && literal(node.arguments[0]) === 'style') {
        if (node.arguments[1]) styleObject(node.arguments[1]); else allStyles()
      }
      if (name === 'assign' && node.arguments[0] && isStyleTarget(node.arguments[0])) {
        for (const arg of node.arguments.slice(1)) styleObject(arg)
      }
      // Classify the receiver, not the shared method name. A primitive string
      // cannot be a stylesheet, even when its contents are not statically known.
      // Continue visiting arguments so callbacks that mutate CSS remain visible.
      const stringReplace = name === 'replace' &&
        (ts.isPropertyAccessExpression(node.expression) || ts.isElementAccessExpression(node.expression)) &&
        (ts.isStringLiteralLike(unwrap(node.expression.expression)) || observer?.isString?.(node.expression.expression))
      if (name === 'insertRule' || name === 'replaceSync' || (name === 'replace' && !stringReplace)) {
        if (node.arguments[0]) styleObject(node.arguments[0])
      }
      // Unknown computed method dispatch may be a canvas operation or style API.
      if (ts.isElementAccessExpression(node.expression) && name === undefined) { addUnknownRendererFeatures(features); observer?.unknown() }
    }
    ts.forEachChild(node, visit)
  }
  // Style object declarations can precede their JSX use. The first pass
  // discovers those bindings; the second also sees earlier mutations.
  visit(source)
  visit(source)
}
