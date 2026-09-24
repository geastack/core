import ts from 'typescript'
import type { StoreArrayFieldPlan, StoreFieldPlan, StoreMethodPlan } from './cpp-stores.js'
import type { FieldAccess } from './cpp-field-access.js'
import type {
  GeaIrComponent,
  GeaIrConstant,
  GeaIrConstantObjectArrayItem,
  GeaIrExpressionObjectField,
  GeaIrKeyedListPayload,
  GeaIrSlot,
  GeaIrStoreField,
  GeaIrStoreValueShape,
} from './types.js'
import {
  cppString,
  cssPropertyName,
  isPxLengthStylePropertyName,
  isBorderWidthStylePropertyName,
  isRecord,
  isRuntimeNumericConstantIdentifier,
  nativeStyleColorExpression,
  opaqueCssHexColor,
  sanitizeCppIdentifier,
  styleDeclarationEnumForPropertyName,
  styleKeywordPropertiesForPropertyName,
  styleKeywordPropertyValuesForPropertyName,
  styleOpaqueColorTargetForPropertyName,
  stylePercentPropertyEnumForPropertyName,
  styleRawNumberPropertyEnumsForPropertyName,
  type StylePropertyValue,
} from './utils.js'

export interface HtmlRoot {
  tag: string
  className?: string
  attrs?: Record<string, string>
}

export interface LoweredSlots {
  stateType: string
  deps: string[]
  fields: StoreFieldPlan[]
  fieldReadPlan: LoweredFieldReadPlan
  staticLines: string[]
  lines: string[]
}

export interface LoweredFieldReadPlan {
  stateType: string
  fields: LoweredFieldRead[]
}

export interface LoweredFieldRead {
  fieldName: string
  fieldType: string
  localName: string
  readerName: string
}

export interface LoweredRowSlots {
  // Lines that run inside `make_row` AFTER the row tree is built. Contains
  // initial style applies and reactive bindings on the root + descendants.
  lines: string[]
  // Per-item-field patches that run inside `patch_row` when that field
  // changes. Each entry says: "when `itemFieldName` changes, apply these
  // lines." The keyed-list emitter wraps them in `if (initial || prev.X !=
  // item.X) {...}` guards.
  patches: LoweredRowPatch[]
  // Lines that depend on scalar store fields outside the row item. They are
  // re-applied whenever the keyed-list binding fires because the caller
  // cannot tell which observed dependency triggered the apply.
  unconditionalPatches: string[]
  // Item fields that affect visible row output, including mixed
  // item-plus-store expressions whose patch lines are unconditional.
  itemFieldNames: string[]
  // Store fields that affect visible row output in addition to the array
  // field itself.
  storeFields: StoreFieldPlan[]
  // Lines that build the row's nested element tree. Run once inside
  // `make_row` BEFORE `lines`. Empty when the row is a single element with
  // no children. When non-empty, every nested element is created via
  // `Document::instance().createView()/createText()` and parented under the
  // row root via `appendChild`. Slot apply lines in `lines` / `patches` can
  // target nested elements by their named local (`__gea_row_0`, etc.).
  treeLines: string[]
  // Lines that attach event listeners (and other one-shot wiring) to the
  // row tree. Run once inside `make_row` after `treeLines` + `lines`. Not
  // re-applied in `patch_row`.
  eventLines: string[]
}

export interface LoweredRowPatch {
  itemFieldName: string
  lines: string[]
}

export type ConstantMap = ReadonlyMap<string, GeaIrConstant>

// Whether the global store cell for `globalName` is the TYPED shared_ptr.
// geatsc core emits store singleton accessors as `std::shared_ptr<Class>&`
// for regular Store subclasses regardless of whether their runtime base is
// compiled or lean. Self-stores are component instances, not exported cells.
export function storeGlobalIsTyped(globalName: string, storeFields: StoreFieldPlan[]): boolean {
  return storeFields.some((field) => field.storeGlobalName === globalName && !field.storeIsSelfStore)
}

export function attributeLines(varName: string, root: HtmlRoot, slots: GeaIrSlot[] = []): string[] {
  void slots
  const lines: string[] = []
  lines.push(`  ${varName}.setTagName(${cppString(root.tag.toLowerCase())});`)
  if (root.className) {
    lines.push(`  ${varName}.classList().set(${cppString(root.className)});`)
  }
  // Forward any other static HTML attributes from the template to the
  // runtime node so framework code (e.g. InputController's momentum opt-in)
  // can read them via getAttribute()/hasAttribute(). Skip `class` and
  // `style` — those have their own dedicated lowering paths.
  const tag = root.tag.toLowerCase()
  if (root.attrs) {
    for (const [name, value] of Object.entries(root.attrs)) {
      const lower = name.toLowerCase()
      if (lower === 'class' || lower === 'style') continue
      // `<img src="face.png">` with a static path: decode the build-embedded
      // asset bytes into an image slot, rather than storing an inert "src"
      // attribute the renderer never decodes.
      if (lower === 'src' && (tag === 'img' || tag === 'image')) {
        lines.push(...imageSrcAssetLines(varName, value))
        continue
      }
      if (lower === 'fit' && (tag === 'img' || tag === 'image')) {
        lines.push(...imageFitApplyLines(varName, value))
        continue
      }
      lines.push(`  ${varName}.setAttribute(${cppString(name)}, ${cppString(value)});`)
    }
  }
  return lines
}

export function rootSetupLines(
  varName: string,
  slots: GeaIrSlot[],
  storeFields: StoreFieldPlan[] = [],
  storeMethods: StoreMethodPlan[] = [],
  fieldAccess?: FieldAccess,
  rootTag?: string,
  constants: ConstantMap = new Map(),
  disposerVar?: string,
): string[] | null {
  const lines: string[] = []
  for (const slot of slots) {
    if (slot.kind === 'style') {
      const lowered = lowerStaticRootStyle(varName, slot, constants)
      if (lowered) lines.push(...lowered)
      continue
    }
    if (slot.kind !== 'attr' && slot.kind !== 'event') continue
    if (slot.walk.length !== 0) return null
    if (slot.kind === 'attr') {
      const lowered = attributeSlotLines(varName, slot, constants)
      if (!lowered) return null
      lines.push(...lowered)
      continue
    }
    const lowered = lowerRootEvent(varName, slot, storeFields, storeMethods, fieldAccess, rootTag, constants, disposerVar)
    if (!lowered) return null
    lines.push(...lowered)
  }
  return lines
}

export function eventSetupLines(
  varName: string,
  slot: GeaIrSlot,
  storeFields: StoreFieldPlan[] = [],
  storeMethods: StoreMethodPlan[] = [],
  fieldAccess?: FieldAccess,
  rootTag?: string,
  constants: ConstantMap = new Map(),
  disposerVar?: string,
): string[] | null {
  return slot.kind === 'event' ? lowerRootEvent(varName, slot, storeFields, storeMethods, fieldAccess, rootTag, constants, disposerVar) : null
}

export function rootSetupSlotsSupported(
  slots: GeaIrSlot[],
  storeFields: StoreFieldPlan[] = [],
  storeMethods: StoreMethodPlan[] = [],
  constants: ConstantMap = new Map(),
): boolean {
  return rootSetupLines('root', slots, storeFields, storeMethods, undefined, undefined, constants) !== null
}

export function isRootSetupSlot(slot: GeaIrSlot, constants: ConstantMap = new Map()): boolean {
  return ((slot.kind === 'attr' || slot.kind === 'event' || slot.kind === 'ref') && slot.walk.length === 0) || isStaticRootStyleSlot(slot, constants)
}

export function childForMount(slot: GeaIrSlot, components: GeaIrComponent[]): GeaIrComponent | null {
  const tag = mountSlotTag(slot)
  return tag ? (components.find((component) => component.exportName === tag) ?? null) : null
}

export function mountSlotTag(slot: GeaIrSlot): string | null {
  const payload = slot.payload
  if (!payload || typeof payload !== 'object' || !('tag' in payload)) return null
  const tag = (payload as { tag?: unknown }).tag
  return typeof tag === 'string' ? tag : null
}

export function mountSlotAttrs(slot: GeaIrSlot): string[] {
  const payload = slot.payload
  if (!payload || typeof payload !== 'object' || !('attrs' in payload)) return []
  const attrs = (payload as { attrs?: unknown }).attrs
  if (!Array.isArray(attrs)) return []
  return attrs
    .map((attr) => {
      if (!attr || typeof attr !== 'object' || !('code' in attr)) return null
      const code = (attr as { code?: unknown }).code
      return typeof code === 'string' ? code : null
    })
    .filter((code): code is string => !!code)
}

export function parseMountAttr(code: string): { name: string; expr: string } | null {
  const trimmed = code.trim()
  const stringAttr = trimmed.match(/^([A-Za-z_$][A-Za-z0-9_$]*)=(["'])(.*)\2$/s)
  if (stringAttr) return { name: stringAttr[1], expr: `${stringAttr[2]}${stringAttr[3]}${stringAttr[2]}` }
  const expressionAttr = trimmed.match(/^([A-Za-z_$][A-Za-z0-9_$]*)=\{([\s\S]*)\}$/)
  if (expressionAttr) return { name: expressionAttr[1], expr: expressionAttr[2].trim() }
  const bareAttr = trimmed.match(/^([A-Za-z_$][A-Za-z0-9_$]*)$/)
  return bareAttr ? { name: bareAttr[1], expr: 'true' } : null
}

export function mountSlotStyleSlot(slot: GeaIrSlot): GeaIrSlot | null {
  for (const attr of mountSlotAttrs(slot)) {
    const parsed = parseMountAttr(attr)
    if (!parsed || parsed.name !== 'style') continue
    const fields = parseExpressionObjectFields(parsed.expr)
    if (!fields) return null
    return {
      index: slot.index,
      kind: 'style',
      walk: [],
      expr: parsed.expr,
      exprObjectFields: fields,
      payload: { attrName: 'style' },
    }
  }
  return null
}

export function mountSlotImageSourceLines(varName: string, slot: GeaIrSlot): string[] | null {
  if (mountSlotTag(slot) !== 'Image') return null
  for (const attr of mountSlotAttrs(slot)) {
    const parsed = parseMountAttr(attr)
    if (!parsed || parsed.name !== 'src') continue
    const imageGlobal = simpleIdentifierValue(parsed.expr)
    if (imageGlobal) return [imageIdApplyLine(varName, imageGlobal)]
    // `<Image src="face.png">` — string-literal path decodes a build-embedded
    // asset. See generate-gea-embedded-assets.mjs.
    const literalPath = stringLiteralValue(parsed.expr)
    if (literalPath !== null) {
      return imageSrcAssetLines(varName, literalPath)
    }
    return null
  }
  return []
}

export function fieldNameForSlot(slot: GeaIrSlot): string | null {
  if (slot.exprPath && slot.exprPath.length > 1) return sanitizeCppIdentifier(slot.exprPath[slot.exprPath.length - 1])
  const match = slot.expr?.match(/\.([A-Za-z_$][A-Za-z0-9_$]*)$/)
  return match ? sanitizeCppIdentifier(match[1]) : null
}

export function keyedListPayload(slot: GeaIrSlot): GeaIrKeyedListPayload | null {
  return isRecord(slot.payload) ? (slot.payload as GeaIrKeyedListPayload) : null
}

export function keyedListKeyExpression(payload: GeaIrKeyedListPayload, itemName: string, field: StoreArrayFieldPlan): string {
  const keyField = keyedListKeyField(payload, itemName, field)
  if (!keyField) return 'std::to_string(__gea_index)'
  const value = `${itemName}.${sanitizeCppIdentifier(keyField.name)}`
  return keyField.shape?.kind === 'literal' && keyField.shape.valueType === 'string' ? value : `gea_cpp_to_string(${value})`
}

export function lowerSlots(slots: GeaIrSlot[], storeFields: StoreFieldPlan[], constants: ConstantMap = new Map()): LoweredSlots | null {
  const stateTypes = new Set<string>()
  const deps: string[] = []
  const fields: StoreFieldPlan[] = []
  const staticLines: string[] = []
  const lines: string[] = []
  const style = lowerStyleSlots(slots, storeFields, constants)
  if (style) {
    stateTypes.add(style.stateType)
    deps.push(...style.deps)
    fields.push(...style.fields)
    staticLines.push(...style.staticLines)
    lines.push(...style.lines)
  }
  const textSlots = slots.filter((slot) => slot.kind === 'text')
  for (const slot of textSlots) {
    if (!slot.directText) return null
    // The `root.setText` emit below is only correct when the text is the
    // ROOT's own direct text child (directText walk = [0]). A longer walk
    // means the text lives on a DESCENDANT — this renderer builds no child
    // structure, so emitting against `root` would drop the subtree and patch
    // the wrong node. Bail instead: nested targets are the template
    // renderer's job (it addresses per-element node vars); anything it can't
    // handle must fall off loudly to the dynamic runtime, not render wrong.
    if (slot.walk.length !== 1) return null
    const text = lowerTextSlot(slot, storeFields)
    if (!text) return null
    stateTypes.add(text.stateType)
    deps.push(...text.deps)
    fields.push(...text.fields)
    lines.push(`root.setText((${text.expr}).c_str());`)
  }
  if (stateTypes.size !== 1 || lines.length === 0) return null
  const uniqueFields = uniqueStoreFields(fields)
  const fieldReadPlan = fieldReadPlanForLoweredSlots([...stateTypes][0], uniqueFields)
  return fieldReadPlan ? { stateType: fieldReadPlan.stateType, deps, fields: uniqueFields, fieldReadPlan, staticLines, lines } : null
}

export function parseHtmlRoot(html: string): HtmlRoot | null {
  const rawTag = html.match(/^<([^>]*)>/)
  const tag = rawTag?.[1].match(/^([A-Za-z][A-Za-z0-9:-]*)/)
  if (!tag) return null
  const attrs = parseStaticAttrs(rawTag?.[1] ?? '')
  return {
    tag: tag[1],
    ...(attrs.class ? { className: attrs.class } : {}),
    attrs,
  }
}

export function lowerRowSlots(
  slots: GeaIrSlot[],
  itemName: string,
  field: StoreArrayFieldPlan,
  constants: ConstantMap = new Map(),
  pathVars: Map<string, string> = new Map([['', 'row']]),
  storeFields: StoreFieldPlan[] = [],
  storeMethods: StoreMethodPlan[] = [],
): LoweredRowSlots {
  const lines: string[] = []
  const patches: LoweredRowPatch[] = []
  const unconditionalPatches: string[] = []
  const itemFieldNames: string[] = []
  const rowStoreFields: StoreFieldPlan[] = []
  const eventLines: string[] = []
  const collectPatch = (lowered: LoweredRowExpr, patchLines: string[]): void => {
    itemFieldNames.push(...lowered.itemFields)
    rowStoreFields.push(...lowered.storeFields)
    if (lowered.storeFields.length > 0) {
      unconditionalPatches.push(...patchLines)
      return
    }
    for (const itemFieldName of lowered.itemFields) patches.push({ itemFieldName, lines: patchLines })
  }
  for (const slot of slots) {
    const target = pathVars.get(slot.walk.join(',')) ?? 'row'
    if (slot.kind === 'style' && slot.exprObjectFields && slot.exprObjectFields.length > 0) {
      for (const styleField of slot.exprObjectFields) {
        const propertyName = cssPropertyName(styleField.name)
        const lowered =
          lowerRowRotateStyleExpression(styleField.expr, propertyName, itemName, field, constants, storeFields) ??
          lowerRowPxLengthStyleExpression(styleField.expr, propertyName, itemName, field, constants, storeFields) ??
          lowerRowPercentLengthStyleExpression(styleField.expr, propertyName, itemName, field, constants, storeFields) ??
          lowerRowOpaqueColorStyleExpression(styleField.expr, propertyName, itemName, field, constants, storeFields) ??
          lowerRowKeywordStyleExpression(styleField.expr, propertyName, itemName, field, constants, storeFields) ??
          lowerRowStyleExpression(styleField.expr, itemName, field, constants, storeFields)
        if (!lowered) continue
        const patchLines = styleApplyLines(target, propertyName, lowered.expr, lowered.shape, lowered.styleUnit)
        lines.push(...patchLines)
        collectPatch(lowered, patchLines)
      }
      continue
    }
    if (slot.kind === 'text' && slot.expr) {
      const lowered = lowerRowStyleExpression(slot.expr, itemName, field, constants, storeFields)
      if (!lowered) continue
      const textExpr = lowered.shape?.kind === 'literal' && lowered.shape.valueType === 'string'
        ? lowered.expr
        : `gea_cpp_to_string(${lowered.expr})`
      const patchLines = [`${target}.setText((${textExpr}).c_str());`]
      lines.push(...patchLines)
      if (lowered.itemFields.length === 0 && lowered.storeFields.length === 0) {
        // Constant text — set once, doesn't need to live in patches.
      } else {
        collectPatch(lowered, patchLines)
      }
      continue
    }
    if (slot.kind === 'class') {
      const classLocal = `__gea_next_class_${slot.index}`
      // Object form: class={{ 'base': true, 'is-active': cond, 'is-hidden': !x }} —
      // build the class string by conditionally appending each truthy key. Mirrors
      // the style-object handling above so a `.map()` row can use the clean class
      // binding instead of a ternary that re-spells the base class in every branch.
      if (slot.exprObjectFields && slot.exprObjectFields.length > 0) {
        const buildLines: string[] = [`std::string ${classLocal};`]
        const combinedItemFields: string[] = []
        const combinedStoreFields: StoreFieldPlan[] = []
        let anyLowered = false
        for (const objField of slot.exprObjectFields) {
          const lowered = lowerRowStyleExpression(objField.expr, itemName, field, constants, storeFields)
          if (!lowered) continue
          anyLowered = true
          combinedItemFields.push(...lowered.itemFields)
          combinedStoreFields.push(...lowered.storeFields)
          buildLines.push(
            `if (${lowered.expr}) { if (!${classLocal}.empty()) ${classLocal} += ' '; ${classLocal} += ${cppString(objField.name)}; }`,
          )
        }
        if (!anyLowered) continue
        const patchLines = [
          ...buildLines,
          `if (${target}.classList().value() != ${classLocal}) ${target}.classList().set(${classLocal});`,
        ]
        lines.push(...patchLines)
        itemFieldNames.push(...combinedItemFields)
        rowStoreFields.push(...combinedStoreFields)
        if (combinedStoreFields.length > 0) {
          unconditionalPatches.push(...patchLines)
        } else {
          for (const itemFieldName of uniqueStrings(combinedItemFields)) patches.push({ itemFieldName, lines: patchLines })
        }
        continue
      }
      if (slot.expr) {
        const lowered = lowerRowStyleExpression(slot.expr, itemName, field, constants, storeFields)
        if (!lowered) continue
        const classExpr = lowered.shape?.kind === 'literal' && lowered.shape.valueType === 'string'
          ? lowered.expr
          : `gea_cpp_to_string(${lowered.expr})`
        const patchLines = [
          `const std::string ${classLocal} = ${classExpr};`,
          `if (${target}.classList().value() != ${classLocal}) ${target}.classList().set(${classLocal});`,
        ]
        lines.push(...patchLines)
        collectPatch(lowered, patchLines)
        continue
      }
      continue
    }
    if (slot.kind === 'event' && slot.expr) {
      const eventLine = lowerRowEventSlot(slot, target, itemName, field, storeMethods)
      if (eventLine) eventLines.push(...eventLine)
      continue
    }
    // `<img src={item.icon}>` in a row: the path is a per-row value, so resolve it
    // at patch time through the runtime asset lookup (loadAssetPath) and set the
    // image. (Literal/global srcs are static and handled by the row's setup pass;
    // a dynamic src reaching the row setup would reject the keyed list entirely.)
    if (slot.kind === 'attr' && slotAttrName(slot) === 'src' && slot.expr && stringLiteralValue(slot.expr) === null) {
      const lowered = lowerRowStyleExpression(slot.expr, itemName, field, constants, storeFields)
      if (!lowered) continue
      const patchLines = [
        `gea::embedded::ui::ImageElement(${target}.id()).imageId(static_cast<int>(gea::host::image.loadAssetPath((${lowered.expr}).c_str())));`,
      ]
      lines.push(...patchLines)
      collectPatch(lowered, patchLines)
      continue
    }
  }
  return {
    lines,
    patches,
    unconditionalPatches: uniqueStrings(unconditionalPatches),
    itemFieldNames: uniqueItemFieldNames(itemFieldNames),
    storeFields: uniqueStoreFields(rowStoreFields),
    treeLines: [],
    eventLines,
  }
}

function lowerRowEventSlot(
  slot: GeaIrSlot,
  target: string,
  itemName: string,
  field: StoreArrayFieldPlan,
  storeMethods: StoreMethodPlan[] = [],
): string[] | null {
  const attrName = slotAttrName(slot)
  const eventType = attrName ? eventTypeFromAttr(attrName) : null
  if (!eventType || !slot.expr) return null
  const arrow = parseArrowFunction(slot.expr)
  if (!arrow) return null

  // Row handlers usually call a store method, optionally passing item fields
  // (e.g. `() => store.selectNote(note.id)`). The item value must be captured
  // at make_row time because the handler fires asynchronously. Walk each
  // statement, capture every `item.field` argument into a per-handler local,
  // and substitute the capture name into the lowered call.
  const captures: string[] = []
  const captureNames: string[] = []
  const bodyLines: string[] = []
  let usesTypedStore = false
  for (let index = 0; index < arrow.body.length; index += 1) {
    const lowered = lowerRowEventStatement(
      arrow.body[index],
      arrow.params,
      itemName,
      field,
      slot.index,
      index,
      captures,
      captureNames,
      storeMethods,
    )
    if (!lowered) return null
    if (lowered.usesTypedStore) usesTypedStore = true
    bodyLines.push(...lowered.lines)
  }
  if (bodyLines.length === 0) return null

  const needsDynamicStore = bodyLines.some((line) => /\bstore\b/.test(line))
  const captureList = [
    ...(needsDynamicStore ? ['store'] : []),
    ...(usesTypedStore ? ['typed_store'] : []),
    ...captureNames,
  ].join(', ')
  // Store the handler on the row NODE's own listener slot — the embedded mirror
  // of gea's DOM delegation, where the handler is an expando on the element
  // (`el.__on_click = h`) rather than a per-row listener on a shared ancestor.
  // Tree::dispatchEvent is the single delegated dispatcher: it walks
  // target -> ancestors, sets event.currentTarget/eventPhase per node, and fires
  // each node's slot — so no body-level containsNode guard or manual currentTarget
  // juggling is needed here. Crucially the handler is reclaimed when the row node
  // is removed (removeNode clears its slot), instead of leaking forever onto body's
  // chained click slot (where a removed row's stale capture would keep firing once
  // its node id was recycled onto a live row).
  return [
    ...captures,
    `${target}.addEventListener(${cppString(eventType)}, [${captureList}](gea::framework::events::PointerEvent &event) mutable {`,
    ...bodyLines.map((line) => `  ${line}`),
    '});',
  ]
}

function lowerRowEventStatement(
  statement: string,
  params: string[],
  itemName: string,
  field: StoreArrayFieldPlan,
  slotIndex: number,
  statementIndex: number,
  captures: string[],
  captureNames: string[],
  storeMethods: StoreMethodPlan[],
): { lines: string[]; usesTypedStore: boolean } | null {
  const call = statement.match(/^([A-Za-z_$][A-Za-z0-9_$]*)\.([A-Za-z_$][A-Za-z0-9_$]*)\((.*)\)$/s)
  if (!call) return null
  const [, receiver, method, rawArgs] = call
  const args = splitTopLevelComma(rawArgs).filter((arg) => arg !== '')
  if (params.includes(receiver) && (method === 'preventDefault' || method === 'stopPropagation')) {
    return args.length > 0 ? null : { lines: [`event.${method}();`], usesTypedStore: false }
  }
  if (params.includes(receiver)) return null

  const loweredArgs: LoweredEventArg[] = []
  for (const arg of args) {
    const number = numericLiteralValue(arg)
    if (number !== null) {
      const direct = `static_cast<double>(${number})`
      loweredArgs.push(eventArg(`gea_cpp_value(${direct})`, direct, 'number'))
      continue
    }
    const string = stringLiteralValue(arg)
    if (string !== null) {
      const direct = `std::string(${cppString(string)})`
      loweredArgs.push(eventArg(`gea_cpp_value(${direct})`, direct, 'string'))
      continue
    }
    // `event.<field>` — a native scalar field of the framework PointerEvent
    // (clientX/clientY/x/y/…), read at dispatch time (NOT captured at make_row).
    // Without this, a row handler like `event => store.sliderDrag(event.clientX)`
    // (a drag slider inside a keyed list) was dropped entirely, so the touch
    // never reached the store.
    const dottedMatch = arg.match(/^([A-Za-z_$][A-Za-z0-9_$]*)\.([A-Za-z_$][A-Za-z0-9_$]*)$/)
    if (dottedMatch && params.includes(dottedMatch[1])) {
      const prop = dottedMatch[2]
      const pointerScalarFields = new Set([
        'clientX', 'clientY', 'x', 'y', 'pageX', 'pageY', 'screenX', 'screenY', 'pointerId',
      ])
      if (!pointerScalarFields.has(prop)) return null
      const direct = `static_cast<double>(event.${prop})`
      loweredArgs.push(eventArg(`gea_cpp_value(${direct})`, direct, 'number'))
      continue
    }
    // `item.field` — capture the field value at make_row time so the handler
    // closes over a stable copy. Captured as gea_cpp_value so the store
    // method's dynamic-call boundary accepts it uniformly.
    const itemFieldMatch = arg.match(/^([A-Za-z_$][A-Za-z0-9_$]*)\.([A-Za-z_$][A-Za-z0-9_$]*)$/)
    if (itemFieldMatch && itemFieldMatch[1] === itemName) {
      const fieldName = sanitizeCppIdentifier(itemFieldMatch[2])
      const itemField = field.itemFields.find((candidate) => sanitizeCppIdentifier(candidate.name) === fieldName)
      if (!itemField) return null
      // Unique across all handlers in the row (slotIndex) — two onClick handlers
      // on sibling row elements both capturing item.id would otherwise both emit
      // `__gea_cap_0_0` into the same make_row scope and conflict.
      const captureName = `__gea_cap_${slotIndex}_${statementIndex}_${captureNames.length}`
      const direct = `${itemName}.${fieldName}`
      const valueType = shapeValueType(itemField.shape)
      captures.push(`  auto ${captureName} = ${direct};`)
      captureNames.push(captureName)
      loweredArgs.push(eventArg(`gea_cpp_key(${captureName})`, captureName, valueType))
      continue
    }
    return null
  }

  const directMethod = storeMethodByReceiver(receiver, method, storeMethods)
  const directArgs = directMethod ? directCallArguments(directMethod, loweredArgs) : null
  if (directMethod && directArgs && directMethod.storeClass === field.storeClass) {
    return {
      lines: [
        `if (typed_store) {`,
        `  (void)typed_store->${directMethod.methodName}(${directArgs.join(', ')});`,
        `}`,
      ],
      usesTypedStore: true,
    }
  }
  if (directMethod && directArgs && receiver !== 'this') {
    const receiverExpr = `__gea_global_${sanitizeCppIdentifier(receiver)}()`
    if (directMethod.globalAccess === 'reference') {
      return {
        lines: [`(void)${receiverExpr}.${directMethod.methodName}(${directArgs.join(', ')});`],
        usesTypedStore: false,
      }
    }
    const storeLocal = `__gea_event_store_${slotIndex}_${statementIndex}`
    return {
      lines: [
        `auto ${storeLocal} = ${receiverExpr};`,
        `if (${storeLocal}) {`,
        `  (void)${storeLocal}->${directMethod.methodName}(${directArgs.join(', ')});`,
        `}`,
      ],
      usesTypedStore: false,
    }
  }

  const receiverExpr = receiver === 'this' ? 'store' : `__gea_global_${sanitizeCppIdentifier(receiver)}()`
  const methodVar = `__gea_method_${statementIndex}`
  // gea_cpp_key boxes a typed global cell for this dynamic lookup (identity
  // for boxed receivers).
  return { lines: [
    `auto ${methodVar} = gea_cpp_key(gea_cpp_key(${receiverExpr}).record_get_literal(${cppString(method)}));`,
    `if (gea::runtime::coerce::to_boolean(${methodVar})) (void)${methodVar}(${loweredArgs.map((arg) => arg.dynamic).join(', ')});`,
  ], usesTypedStore: false }
}


export function parseExpressionObjectFields(expr: string): GeaIrExpressionObjectField[] | null {
  const text = stripOuterParens(expr.trim())
  if (!text.startsWith('{') || !text.endsWith('}')) return null
  const inner = text.slice(1, -1).trim()
  if (!inner) return []
  const fields: GeaIrExpressionObjectField[] = []
  for (const part of splitTopLevelComma(inner)) {
    if (!part || part.startsWith('...')) return null
    const colon = findTopLevelOperator(part, ':')
    if (colon < 0) return null
    const rawName = part.slice(0, colon).trim()
    const name = objectKeyName(rawName)
    if (!name) return null
    fields.push({ name, expr: part.slice(colon + 1).trim() })
  }
  return fields
}

export function constantExpression(
  name: string,
  constants: ConstantMap,
): { expr: string; shape: GeaIrStoreValueShape } | null {
  const constant = constants.get(name)
  if (!constant) return null
  if (constant.valueType === 'object-array') return null
  if (constant.valueType === 'number') {
    return { expr: `static_cast<double>(${constant.value})`, shape: { kind: 'literal', valueType: 'number' } }
  }
  if (constant.valueType === 'string') {
    return { expr: `std::string(${cppString(constant.value)})`, shape: { kind: 'literal', valueType: 'string' } }
  }
  if (constant.valueType === 'boolean') {
    return { expr: constant.value, shape: { kind: 'literal', valueType: 'boolean' } }
  }
  return { expr: 'gea_cpp_value::null_v()', shape: { kind: 'literal', valueType: 'null' } }
}

export function objectArrayConstant(
  name: string | undefined,
  constants: ConstantMap,
): GeaIrConstantObjectArrayItem[] | null {
  if (!name || !/^[A-Za-z_$][A-Za-z0-9_$]*$/.test(name.trim())) return null
  const constant = constants.get(name.trim())
  if (!constant || constant.valueType !== 'object-array' || !constant.items) return null
  return constant.items
}

interface LoweredRowExpr {
  expr: string
  shape: GeaIrStoreValueShape | null
  itemFields: string[]
  storeFields: StoreFieldPlan[]
  styleUnit?: 'percent' | 'color' | 'keyword' | 'px'
}

function lowerRowStyleExpression(
  expr: string,
  itemName: string,
  field: StoreArrayFieldPlan,
  constants: ConstantMap,
  storeFields: StoreFieldPlan[],
): LoweredRowExpr | null {
  const source = ts.createSourceFile('row-style.ts', `(${expr});`, ts.ScriptTarget.Latest, false)
  const stmt = source.statements[0]
  if (!stmt || !ts.isExpressionStatement(stmt)) return null
  return lowerRowExpressionNode(stmt.expression, itemName, field, constants, storeFields)
}

function lowerRowPxLengthStyleExpression(
  expr: string,
  propertyName: string,
  itemName: string,
  field: StoreArrayFieldPlan,
  constants: ConstantMap,
  storeFields: StoreFieldPlan[],
): LoweredRowExpr | null {
  if (!isPxLengthStylePropertyName(propertyName)) return null
  const source = ts.createSourceFile('row-style.ts', `(${expr});`, ts.ScriptTarget.Latest, false)
  const stmt = source.statements[0]
  if (!stmt || !ts.isExpressionStatement(stmt)) return null
  const node = unwrapExpressionNode(stmt.expression)
  if (!ts.isTemplateExpression(node)) return null
  if (node.head.text !== '' || node.templateSpans.length !== 1) return null
  const span = node.templateSpans[0]
  if (span.literal.text !== 'px') return null
  const lowered = lowerRowExpressionNode(span.expression, itemName, field, constants, storeFields)
  if (!lowered) return null
  return {
    expr: loweredRowAsNumber(lowered),
    shape: { kind: 'literal', valueType: 'number' },
    styleUnit: isBorderWidthStylePropertyName(propertyName) ? 'px' : undefined,
    itemFields: lowered.itemFields,
    storeFields: lowered.storeFields,
  }
}

function lowerRowPercentLengthStyleExpression(
  expr: string,
  propertyName: string,
  itemName: string,
  field: StoreArrayFieldPlan,
  constants: ConstantMap,
  storeFields: StoreFieldPlan[],
): LoweredRowExpr | null {
  if (!stylePercentPropertyEnumForPropertyName(propertyName)) return null
  const source = ts.createSourceFile('row-style.ts', `(${expr});`, ts.ScriptTarget.Latest, false)
  const stmt = source.statements[0]
  if (!stmt || !ts.isExpressionStatement(stmt)) return null
  const node = unwrapExpressionNode(stmt.expression)
  if (!ts.isTemplateExpression(node)) return null
  if (node.head.text !== '' || node.templateSpans.length !== 1) return null
  const span = node.templateSpans[0]
  if (span.literal.text !== '%') return null
  const lowered = lowerRowExpressionNode(span.expression, itemName, field, constants, storeFields)
  if (!lowered) return null
  return {
    expr: loweredRowAsNumber(lowered),
    shape: { kind: 'literal', valueType: 'number' },
    itemFields: lowered.itemFields,
    storeFields: lowered.storeFields,
    styleUnit: 'percent',
  }
}

function lowerRowRotateStyleExpression(
  expr: string,
  propertyName: string,
  itemName: string,
  field: StoreArrayFieldPlan,
  constants: ConstantMap,
  storeFields: StoreFieldPlan[],
): LoweredRowExpr | null {
  if (!isRotateTransformPropertyName(propertyName)) return null
  const source = ts.createSourceFile('row-style.ts', `(${expr});`, ts.ScriptTarget.Latest, false)
  const stmt = source.statements[0]
  if (!stmt || !ts.isExpressionStatement(stmt)) return null
  const node = unwrapExpressionNode(stmt.expression)
  if (!ts.isTemplateExpression(node)) return null
  const degrees = rotateDegreesTemplateExpression(node, propertyName)
  if (!degrees) return null
  const lowered = lowerRowExpressionNode(degrees, itemName, field, constants, storeFields)
  if (!lowered) return null
  return {
    expr: loweredRowAsNumber(lowered),
    shape: { kind: 'literal', valueType: 'number' },
    itemFields: lowered.itemFields,
    storeFields: lowered.storeFields,
  }
}

function lowerRowOpaqueColorStyleExpression(
  expr: string,
  propertyName: string,
  itemName: string,
  field: StoreArrayFieldPlan,
  constants: ConstantMap,
  storeFields: StoreFieldPlan[],
): LoweredRowExpr | null {
  if (!styleOpaqueColorTargetForPropertyName(propertyName)) return null
  const source = ts.createSourceFile('row-style.ts', `(${expr});`, ts.ScriptTarget.Latest, false)
  const stmt = source.statements[0]
  if (!stmt || !ts.isExpressionStatement(stmt)) return null
  return lowerRowOpaqueColorStyleExpressionNode(unwrapExpressionNode(stmt.expression), itemName, field, constants, storeFields)
}

function lowerRowOpaqueColorStyleExpressionNode(
  node: ts.Expression,
  itemName: string,
  field: StoreArrayFieldPlan,
  constants: ConstantMap,
  storeFields: StoreFieldPlan[],
): LoweredRowExpr | null {
  const literal = opaqueHexColorExpressionForNode(node, constants)
  if (literal) {
    return {
      expr: literal,
      shape: { kind: 'literal', valueType: 'number' },
      itemFields: [],
      storeFields: [],
      styleUnit: 'color',
    }
  }
  if (!ts.isConditionalExpression(node)) return null
  const test = lowerRowExpressionNode(node.condition, itemName, field, constants, storeFields)
  const consequent = opaqueHexColorExpressionForNode(node.whenTrue, constants)
  const alternate = opaqueHexColorExpressionForNode(node.whenFalse, constants)
  if (!test || !consequent || !alternate) return null
  return {
    expr: `(${loweredRowAsBoolean(test)} ? ${consequent} : ${alternate})`,
    shape: { kind: 'literal', valueType: 'number' },
    itemFields: test.itemFields,
    storeFields: test.storeFields,
    styleUnit: 'color',
  }
}

function lowerRowKeywordStyleExpression(
  expr: string,
  propertyName: string,
  itemName: string,
  field: StoreArrayFieldPlan,
  constants: ConstantMap,
  storeFields: StoreFieldPlan[],
): LoweredRowExpr | null {
  if (!styleKeywordPropertiesForPropertyName(propertyName)) return null
  const source = ts.createSourceFile('row-style.ts', `(${expr});`, ts.ScriptTarget.Latest, false)
  const stmt = source.statements[0]
  if (!stmt || !ts.isExpressionStatement(stmt)) return null
  return lowerRowKeywordStyleExpressionNode(unwrapExpressionNode(stmt.expression), propertyName, itemName, field, constants, storeFields)
}

function lowerRowKeywordStyleExpressionNode(
  node: ts.Expression,
  propertyName: string,
  itemName: string,
  field: StoreArrayFieldPlan,
  constants: ConstantMap,
  storeFields: StoreFieldPlan[],
): LoweredRowExpr | null {
  const literal = keywordCommonValue(keywordEntriesForNode(propertyName, node, constants))
  if (literal !== null) {
    return {
      expr: String(literal),
      shape: { kind: 'literal', valueType: 'number' },
      itemFields: [],
      storeFields: [],
      styleUnit: 'keyword',
    }
  }
  if (!ts.isConditionalExpression(node)) return null
  const test = lowerRowExpressionNode(node.condition, itemName, field, constants, storeFields)
  const consequent = keywordEntriesForNode(propertyName, node.whenTrue, constants)
  const alternate = keywordEntriesForNode(propertyName, node.whenFalse, constants)
  const consequentValue = keywordCommonValue(consequent)
  const alternateValue = keywordCommonValue(alternate)
  if (!test || consequentValue === null || alternateValue === null || !keywordEntriesCompatible(consequent, alternate)) return null
  return {
    expr: `(${loweredRowAsBoolean(test)} ? ${consequentValue} : ${alternateValue})`,
    shape: { kind: 'literal', valueType: 'number' },
    itemFields: test.itemFields,
    storeFields: test.storeFields,
    styleUnit: 'keyword',
  }
}

function unwrapExpressionNode(node: ts.Expression): ts.Expression {
  let current = node
  while (
    ts.isParenthesizedExpression(current) ||
    ts.isAsExpression(current) ||
    ts.isTypeAssertionExpression(current) ||
    ts.isNonNullExpression(current) ||
    ts.isSatisfiesExpression(current)
  ) {
    current = current.expression
  }
  return current
}

function lowerRowExpressionNode(
  node: ts.Expression,
  itemName: string,
  field: StoreArrayFieldPlan,
  constants: ConstantMap,
  storeFields: StoreFieldPlan[],
): LoweredRowExpr | null {
  if (ts.isParenthesizedExpression(node)) return lowerRowExpressionNode(node.expression, itemName, field, constants, storeFields)
  if (
    ts.isAsExpression(node) ||
    ts.isTypeAssertionExpression(node) ||
    ts.isNonNullExpression(node) ||
    ts.isSatisfiesExpression(node)
  ) {
    return lowerRowExpressionNode(node.expression, itemName, field, constants, storeFields)
  }
  if (ts.isStringLiteral(node) || ts.isNoSubstitutionTemplateLiteral(node)) {
    return { expr: `std::string(${cppString(node.text)})`, shape: { kind: 'literal', valueType: 'string' }, itemFields: [], storeFields: [] }
  }
  // Template literal with substitutions inside a `.map()` row, e.g.
  // `{`${row.ms} ms`}`. Without this case the slot lowered to null and was
  // silently dropped (no setText emitted) — the row's text rendered empty. Lower
  // to a std::string concatenation of head + coerced spans + tails; the leading
  // std::string makes the `+` chain well-typed even when the head is empty.
  if (ts.isTemplateExpression(node)) {
    const itemFields: string[] = []
    const spanStoreFields: StoreFieldPlan[] = []
    const parts: string[] = [`std::string(${cppString(node.head.text)})`]
    for (const span of node.templateSpans) {
      const part = lowerRowExpressionNode(span.expression, itemName, field, constants, storeFields)
      if (!part) return null
      itemFields.push(...part.itemFields)
      spanStoreFields.push(...part.storeFields)
      parts.push(loweredRowAsString(part))
      if (span.literal.text.length > 0) parts.push(`std::string(${cppString(span.literal.text)})`)
    }
    return {
      expr: `(${parts.join(' + ')})`,
      shape: { kind: 'literal', valueType: 'string' },
      itemFields: uniqueItemFieldNames(itemFields),
      storeFields: uniqueStoreFields(spanStoreFields),
    }
  }
  if (ts.isNumericLiteral(node)) {
    return { expr: `static_cast<double>(${node.text})`, shape: { kind: 'literal', valueType: 'number' }, itemFields: [], storeFields: [] }
  }
  if (node.kind === ts.SyntaxKind.TrueKeyword) return { expr: 'true', shape: { kind: 'literal', valueType: 'boolean' }, itemFields: [], storeFields: [] }
  if (node.kind === ts.SyntaxKind.FalseKeyword) return { expr: 'false', shape: { kind: 'literal', valueType: 'boolean' }, itemFields: [], storeFields: [] }
  if (ts.isIdentifier(node)) {
    const constant = constantExpression(node.text, constants)
    if (constant) return { ...constant, itemFields: [], storeFields: [] }
    // SCREAMING_SNAKE_CASE module constants (e.g. BLOCK_SIZE) are assigned once
    // at module init and emitted as in-scope C++ globals. The non-keyed path
    // (attributeValueExpression) already references them directly; mirror that
    // here so constant inline-style props on `.map`'d rows — e.g.
    // `width: BLOCK_SIZE` in button-tetris — are emitted instead of dropped,
    // which previously left mapped rows with no width/height.
    if (isRuntimeNumericConstantIdentifier(node.text)) {
      return {
        expr: `static_cast<double>(${node.text})`,
        shape: { kind: 'literal', valueType: 'number' },
        itemFields: [],
        storeFields: [],
      }
    }
    return null
  }
  if (ts.isPropertyAccessExpression(node)) {
    if (!ts.isIdentifier(node.expression)) return null
    const receiver = node.expression.text
    if (receiver !== itemName) {
      const storeField = storeFieldByReceiver(receiver, node.name.text, storeFields)
      if (!storeField?.readerName || storeField.storeClass !== field.storeClass) return null
      return {
        expr: `${storeField.readerName}(store)`,
        shape: shapeForStoreFieldPlan(storeField),
        itemFields: [],
        storeFields: [storeField],
      }
    }
    const itemFieldName = sanitizeCppIdentifier(node.name.text)
    const itemField = field.itemFields.find((candidate) => sanitizeCppIdentifier(candidate.name) === itemFieldName)
    return {
      expr: `${itemName}.${itemFieldName}`,
      shape: valueShapeForStoreField(itemField),
      itemFields: [itemFieldName],
      storeFields: [],
    }
  }
  if (ts.isPrefixUnaryExpression(node)) {
    const operand = lowerRowExpressionNode(node.operand, itemName, field, constants, storeFields)
    if (!operand) return null
    if (node.operator === ts.SyntaxKind.MinusToken) {
      return { expr: `(-${loweredRowAsNumber(operand)})`, shape: { kind: 'literal', valueType: 'number' }, itemFields: operand.itemFields, storeFields: operand.storeFields }
    }
    if (node.operator === ts.SyntaxKind.PlusToken) {
      return { expr: `(+${loweredRowAsNumber(operand)})`, shape: { kind: 'literal', valueType: 'number' }, itemFields: operand.itemFields, storeFields: operand.storeFields }
    }
    if (node.operator === ts.SyntaxKind.ExclamationToken) {
      return { expr: `(!${loweredRowAsBoolean(operand)})`, shape: { kind: 'literal', valueType: 'boolean' }, itemFields: operand.itemFields, storeFields: operand.storeFields }
    }
    return null
  }
  if (ts.isConditionalExpression(node)) {
    const test = lowerRowExpressionNode(node.condition, itemName, field, constants, storeFields)
    const consequent = lowerRowExpressionNode(node.whenTrue, itemName, field, constants, storeFields)
    const alternate = lowerRowExpressionNode(node.whenFalse, itemName, field, constants, storeFields)
    if (!test || !consequent || !alternate) return null
    const kind = resultKindFor(consequent, alternate)
    return {
      expr: `(${loweredRowAsBoolean(test)} ? ${loweredRowAsKind(consequent, kind)} : ${loweredRowAsKind(alternate, kind)})`,
      shape: { kind: 'literal', valueType: kind },
      itemFields: uniqueItemFieldNames([...test.itemFields, ...consequent.itemFields, ...alternate.itemFields]),
      storeFields: uniqueStoreFields([...test.storeFields, ...consequent.storeFields, ...alternate.storeFields]),
    }
  }
  if (ts.isBinaryExpression(node)) {
    return lowerRowBinaryExpression(node, itemName, field, constants, storeFields)
  }
  if (ts.isCallExpression(node)) {
    return lowerRowCallExpression(node, itemName, field, constants, storeFields)
  }
  return null
}

function lowerRowBinaryExpression(
  node: ts.BinaryExpression,
  itemName: string,
  field: StoreArrayFieldPlan,
  constants: ConstantMap,
  storeFields: StoreFieldPlan[],
): LoweredRowExpr | null {
  const op = node.operatorToken.kind
  const left = lowerRowExpressionNode(node.left, itemName, field, constants, storeFields)
  const right = lowerRowExpressionNode(node.right, itemName, field, constants, storeFields)
  if (!left || !right) return null
  const itemFields = uniqueItemFieldNames([...left.itemFields, ...right.itemFields])
  const deps = uniqueStoreFields([...left.storeFields, ...right.storeFields])
  const boolean = { kind: 'literal' as const, valueType: 'boolean' as const }
  const number = { kind: 'literal' as const, valueType: 'number' as const }
  const string = { kind: 'literal' as const, valueType: 'string' as const }

  switch (op) {
    case ts.SyntaxKind.EqualsEqualsEqualsToken:
      return { expr: loweredRowEqualityExpression(left, right, '===', false), shape: boolean, itemFields, storeFields: deps }
    case ts.SyntaxKind.EqualsEqualsToken:
      return { expr: loweredRowEqualityExpression(left, right, '==', false), shape: boolean, itemFields, storeFields: deps }
    case ts.SyntaxKind.ExclamationEqualsEqualsToken:
      return { expr: loweredRowEqualityExpression(left, right, '===', true), shape: boolean, itemFields, storeFields: deps }
    case ts.SyntaxKind.ExclamationEqualsToken:
      return { expr: loweredRowEqualityExpression(left, right, '==', true), shape: boolean, itemFields, storeFields: deps }
    case ts.SyntaxKind.LessThanToken:
      return { expr: `(${loweredRowAsNumber(left)} < ${loweredRowAsNumber(right)})`, shape: boolean, itemFields, storeFields: deps }
    case ts.SyntaxKind.LessThanEqualsToken:
      return { expr: `(${loweredRowAsNumber(left)} <= ${loweredRowAsNumber(right)})`, shape: boolean, itemFields, storeFields: deps }
    case ts.SyntaxKind.GreaterThanToken:
      return { expr: `(${loweredRowAsNumber(left)} > ${loweredRowAsNumber(right)})`, shape: boolean, itemFields, storeFields: deps }
    case ts.SyntaxKind.GreaterThanEqualsToken:
      return { expr: `(${loweredRowAsNumber(left)} >= ${loweredRowAsNumber(right)})`, shape: boolean, itemFields, storeFields: deps }
    case ts.SyntaxKind.AmpersandAmpersandToken:
      return { expr: `(${loweredRowAsBoolean(left)} && ${loweredRowAsBoolean(right)})`, shape: boolean, itemFields, storeFields: deps }
    case ts.SyntaxKind.BarBarToken:
      return { expr: `(${loweredRowAsBoolean(left)} || ${loweredRowAsBoolean(right)})`, shape: boolean, itemFields, storeFields: deps }
    case ts.SyntaxKind.PlusToken: {
      const leftIsString = left.shape?.kind === 'literal' && left.shape.valueType === 'string'
      const rightIsString = right.shape?.kind === 'literal' && right.shape.valueType === 'string'
      if (leftIsString || rightIsString) {
        return { expr: `(${loweredRowAsString(left)} + ${loweredRowAsString(right)})`, shape: string, itemFields, storeFields: deps }
      }
      return { expr: `(${loweredRowAsNumber(left)} + ${loweredRowAsNumber(right)})`, shape: number, itemFields, storeFields: deps }
    }
    case ts.SyntaxKind.MinusToken:
      return { expr: `(${loweredRowAsNumber(left)} - ${loweredRowAsNumber(right)})`, shape: number, itemFields, storeFields: deps }
    case ts.SyntaxKind.AsteriskToken:
      return { expr: `(${loweredRowAsNumber(left)} * ${loweredRowAsNumber(right)})`, shape: number, itemFields, storeFields: deps }
    case ts.SyntaxKind.SlashToken:
      return { expr: `(${loweredRowAsNumber(left)} / ${loweredRowAsNumber(right)})`, shape: number, itemFields, storeFields: deps }
    case ts.SyntaxKind.PercentToken:
      return { expr: `std::fmod(${loweredRowAsNumber(left)}, ${loweredRowAsNumber(right)})`, shape: number, itemFields, storeFields: deps }
    default:
      return null
  }
}

function loweredRowEqualityExpression(left: LoweredRowExpr, right: LoweredRowExpr, mode: '===' | '==', negate: boolean): string {
  const leftKind = primitiveLiteralKind(left)
  const rightKind = primitiveLiteralKind(right)
  if (leftKind && leftKind === rightKind) {
    const op = negate ? '!=' : '=='
    return `(${loweredRowAsKind(left, leftKind)} ${op} ${loweredRowAsKind(right, rightKind)})`
  }
  const fn = mode === '===' ? 'gea_cpp_strict_equals' : 'gea_cpp_loose_equals'
  const expr = `${fn}(gea_cpp_key(${left.expr}), gea_cpp_key(${right.expr}))`
  return negate ? `(!${expr})` : expr
}

function primitiveLiteralKind(lowered: LoweredRowExpr): 'string' | 'number' | 'boolean' | null {
  if (lowered.shape?.kind !== 'literal') return null
  if (lowered.shape.valueType === 'string' || lowered.shape.valueType === 'number' || lowered.shape.valueType === 'boolean') {
    return lowered.shape.valueType
  }
  return null
}

function lowerRowCallExpression(
  node: ts.CallExpression,
  itemName: string,
  field: StoreArrayFieldPlan,
  constants: ConstantMap,
  storeFields: StoreFieldPlan[],
): LoweredRowExpr | null {
  if (!ts.isPropertyAccessExpression(node.expression)) return null
  if (!ts.isIdentifier(node.expression.expression) || node.expression.expression.text !== 'Math') return null
  const method = node.expression.name.text
  const args = node.arguments.map((arg) => lowerRowExpressionNode(arg, itemName, field, constants, storeFields))
  if (args.some((arg) => arg === null)) return null
  const lowered = args as LoweredRowExpr[]
  const itemFields = uniqueItemFieldNames(lowered.flatMap((arg) => arg.itemFields))
  const deps = uniqueStoreFields(lowered.flatMap((arg) => arg.storeFields))
  const number = { kind: 'literal' as const, valueType: 'number' as const }
  if ((method === 'abs' || method === 'floor' || method === 'ceil' || method === 'round') && lowered.length === 1) {
    const fn = method === 'abs' ? 'std::abs' : method === 'floor' ? 'std::floor' : method === 'ceil' ? 'std::ceil' : 'std::round'
    return { expr: `${fn}(${loweredRowAsNumber(lowered[0])})`, shape: number, itemFields, storeFields: deps }
  }
  if ((method === 'min' || method === 'max') && lowered.length >= 2) {
    const fn = method === 'min' ? 'std::min' : 'std::max'
    let expr = loweredRowAsNumber(lowered[0])
    for (let i = 1; i < lowered.length; i += 1) expr = `${fn}(${expr}, ${loweredRowAsNumber(lowered[i])})`
    return { expr, shape: number, itemFields, storeFields: deps }
  }
  return null
}

function loweredRowAsNumber(lowered: LoweredRowExpr): string {
  if (lowered.shape?.kind === 'literal' && lowered.shape.valueType === 'number') return lowered.expr
  return `gea::runtime::coerce::to_number(gea_cpp_key(${lowered.expr}))`
}

function loweredRowAsBoolean(lowered: LoweredRowExpr): string {
  if (lowered.shape?.kind === 'literal' && lowered.shape.valueType === 'boolean') return lowered.expr
  if (lowered.shape?.kind === 'literal' && lowered.shape.valueType === 'number') return `(${lowered.expr} != static_cast<double>(0))`
  if (lowered.shape?.kind === 'literal' && lowered.shape.valueType === 'string') return `(!${lowered.expr}.empty())`
  return `gea::runtime::coerce::to_boolean(gea_cpp_key(${lowered.expr}))`
}

function loweredRowAsString(lowered: LoweredRowExpr): string {
  if (lowered.shape?.kind === 'literal' && lowered.shape.valueType === 'string') return lowered.expr
  if (lowered.shape?.kind === 'literal' && lowered.shape.valueType === 'number') return `gea_cpp_to_string(${lowered.expr})`
  if (lowered.shape?.kind === 'literal' && lowered.shape.valueType === 'boolean') return `gea_cpp_to_string(${lowered.expr})`
  return `gea_cpp_to_string(gea_cpp_key(${lowered.expr}))`
}

function loweredRowAsKind(lowered: LoweredRowExpr, kind: 'string' | 'number' | 'boolean'): string {
  if (kind === 'string') return loweredRowAsString(lowered)
  if (kind === 'number') return loweredRowAsNumber(lowered)
  return loweredRowAsBoolean(lowered)
}

function resultKindFor(a: LoweredRowExpr, b: LoweredRowExpr): 'string' | 'number' | 'boolean' {
  const aType = a.shape?.kind === 'literal' ? a.shape.valueType : null
  const bType = b.shape?.kind === 'literal' ? b.shape.valueType : null
  if (aType === 'string' || bType === 'string') return 'string'
  if (aType === 'number' || bType === 'number') return 'number'
  if (aType === 'boolean' && bType === 'boolean') return 'boolean'
  return 'string'
}

// Image globals are native `gea::host::GeaEmbeddedImage&` accessors (loadImage
// returns the concrete struct), so the slot id is a plain `.id` field read — no
// gea_cpp_value boxing. mountedRendererGlobalAccessorDeclarations keys off the
// `().id` shape to forward-declare these with the image type.
function imageIdApplyLine(varName: string, imageGlobal: string): string {
  return `  gea::embedded::ui::ImageElement(${varName}.id()).imageId(__gea_global_${sanitizeCppIdentifier(imageGlobal)}().id);`
}

function imageFitValue(fit: string): number | null {
  switch (fit) {
    case 'fill':
      return 0
    case 'contain':
      return 1
    case 'cover':
      return 2
    case 'none':
      return 3
    case 'scale-down':
      return 4
    default:
      return null
  }
}

function imageFitApplyLines(varName: string, fit: string): string[] {
  const value = imageFitValue(fit)
  if (value === null) return [`  ${varName}.setAttribute("fit", ${cppString(fit)});`]
  return [`  gea::embedded::ui::ImageElement(${varName}.id()).fit(${value});`]
}

// Keep identical to canonicalAssetPath() / assetSymbol() in
// scripts/generate-gea-embedded-assets.mjs.
function canonicalAssetPath(relPath: string): string {
  const normalized = relPath.startsWith('/') ? relPath.slice(1) : relPath
  return normalized.startsWith('public/') ? normalized.slice('public/'.length) : normalized
}

function assetSymbol(relPath: string): string {
  return `gea_asset_${canonicalAssetPath(relPath).replace(/[^A-Za-z0-9]/g, '_')}`
}

// `<img src="path">` (string-literal path) → decode the build-embedded asset
// bytes into the image slot. The codegen references the generated
// `gea_asset_<path>` symbol directly; that reference is what keeps the asset
// object in the link (a static-init registry gets dropped from ESP-IDF's
// archive link). The symbol is `extern "C"` (defined in
// scripts/generate-gea-embedded-assets.mjs) and forward-declared at namespace
// scope by mountedRendererAssetDeclarations() in cpp-ir.ts — `extern "C"`
// makes it resolve identically whether the enclosing `gea_ir` is global or
// (in an isolated multi-program link) wrapped in an anonymous namespace. A plain C++
// block-scope `extern` here would instead bind to `(anonymous
// namespace)::gea_ir::gea_asset_<path>` and fail to link.
export function imageSrcAssetIdExpression(relPath: string): string {
  const sym = assetSymbol(relPath)
  return `gea::host::image.loadBytesPtr(${sym}, static_cast<std::size_t>(${sym}_len))`
}

function imageSrcAssetLines(varName: string, relPath: string): string[] {
  return [
    `  gea::embedded::ui::ImageElement(${varName}.id()).imageId(static_cast<int>(${imageSrcAssetIdExpression(relPath)}));`,
  ]
}

export function unique<T>(values: T[]): T | null {
  return values.length === 1 ? values[0] : null
}

export function fieldReadPlanForLoweredSlots(stateType: string, fields: StoreFieldPlan[]): LoweredFieldReadPlan | null {
  const reads: LoweredFieldRead[] = []
  for (const field of fields) {
    if (!field.readerName) return null
    reads.push({
      fieldName: field.fieldName,
      fieldType: field.fieldType,
      localName: storeFieldLocalName(field.fieldName),
      readerName: field.readerName,
    })
  }
  return reads.length > 0 ? { stateType, fields: reads } : null
}

export function fieldReadLines(plan: LoweredFieldReadPlan, storeExpression = 'store'): string[] {
  return plan.fields.map((field) => `const auto ${field.localName} = ${field.readerName}(${storeExpression});`)
}

function lowerStyleSlots(
  slots: GeaIrSlot[],
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
): { stateType: string; deps: string[]; fields: StoreFieldPlan[]; staticLines: string[]; lines: string[] } | null {
  const stateTypes = new Set<string>()
  const deps: string[] = []
  const fields: StoreFieldPlan[] = []
  const staticLines: string[] = []
  const lines: string[] = []
  for (const slot of slots) {
    if (slot.kind !== 'style' || !slot.exprObjectFields) continue
    // Reactive style slots are applied to `root` below; a non-empty walk
    // means the slot styles a DESCENDANT element this renderer never builds.
    // Bail so the component falls off this root-only fast path instead of
    // restyling the wrong node (the template renderer handles nested slots).
    if (slot.walk.length !== 0) return null
    for (const field of slot.exprObjectFields) {
      const propertyName = cssPropertyName(field.name)
      const staticFieldLines = staticRootStyleApplyLines('root', propertyName, field.expr, constants)
      if (staticFieldLines) {
        staticLines.push(...staticFieldLines)
        continue
      }
      const lowered =
        lowerStoreRotateStyleExpression(field, propertyName, storeFields) ??
        lowerStorePxLengthStyleExpression(field, propertyName, storeFields) ??
        lowerStorePercentLengthStyleExpression(field, propertyName, storeFields) ??
        lowerStoreOpaqueColorStyleExpression(field, propertyName, storeFields, constants) ??
        lowerStoreKeywordStyleExpression(field, propertyName, storeFields, constants) ??
        lowerStoreExpression(field, storeFields)
      if (!lowered) return null
      stateTypes.add(lowered.store.stateType)
      deps.push(lowered.store.fieldName)
      fields.push(lowered.store)
      lines.push(...styleApplyLines('root', propertyName, lowered.expr, lowered.shape ?? shapeForStoreFieldPlan(lowered.store), lowered.styleUnit))
    }
  }
  if (stateTypes.size !== 1 || lines.length === 0) return null
  return { stateType: [...stateTypes][0], deps, fields: uniqueStoreFields(fields), staticLines, lines }
}

export function attributeSlotLines(varName: string, slot: GeaIrSlot, constants: ConstantMap = new Map()): string[] | null {
  const attrName = slotAttrName(slot)
  if (!attrName || slot.expr === undefined) return null

  const number = numericLiteralValue(slot.expr)
  if (attrName === 'pressId') return number === null ? null : [`  ${varName}.setPressId(${number});`]
  if (attrName === 'pressValue') return number === null ? null : [`  ${varName}.setPressValue(${number});`]
  if (attrName === 'src') {
    const imageGlobal = simpleIdentifierValue(slot.expr)
    if (imageGlobal) {
      return [imageIdApplyLine(varName, imageGlobal)]
    }
    // `<img src="face.png">` — a string-literal path decodes a build-embedded
    // asset straight into an image slot. No byte array in the bundle; the bytes
    // are `.rodata` in the app binary. See generate-gea-embedded-assets.mjs.
    const literalPath = stringLiteralValue(slot.expr)
    if (literalPath !== null) {
      return imageSrcAssetLines(varName, literalPath)
    }
  }
  if (attrName === 'fit') {
    const literalFit = stringLiteralValue(slot.expr)
    if (literalFit !== null) return imageFitApplyLines(varName, literalFit)
  }

  const value = attributeValueExpression(slot.expr, constants)
  if (!value) return null
  return [`  ${varName}.setAttribute(${cppString(attrName)}, ${value});`]
}

function lowerStaticRootStyle(varName: string, slot: GeaIrSlot, constants: ConstantMap): string[] | null {
  if (!isStaticRootStyleSlot(slot, constants) || !slot.exprObjectFields) return null
  const lines: string[] = []
  for (const field of slot.exprObjectFields) {
    const propertyName = cssPropertyName(field.name)
    const lowered = staticRootStyleApplyLines(varName, propertyName, field.expr, constants)
    if (!lowered) return null
    lines.push(...lowered.map((line) => `  ${line}`))
  }
  return lines
}

function staticRootStyleApplyLines(varName: string, propertyName: string, expr: string, constants: ConstantMap): string[] | null {
  const number = staticRootStyleNumberExpression(expr, constants)
  if (number !== null) {
    const direct = directNumberStyleApplyLine(varName, propertyName, number)
    return [
      direct ??
        `gea::embedded::ui::StyleSheet::instance().applyNumberProperty(${varName}, ${stylePropertyArgument(propertyName)}, static_cast<double>(${number}));`,
    ]
  }
  const string = staticRootStyleStringValue(expr, constants)
  if (string !== null) {
    const direct = directStringStyleApplyLines(varName, propertyName, string)
    return direct ?? [
      `gea::embedded::ui::StyleSheet::instance().applyProperty(${varName}, ${cppString(propertyName)}, std::string(${cppString(string)}));`,
    ]
  }
  return null
}

function isStaticRootStyleSlot(slot: GeaIrSlot, constants: ConstantMap): boolean {
  if (slot.kind !== 'style' || slot.walk.length !== 0 || !slot.exprObjectFields || slot.exprObjectFields.length === 0) return false
  return slot.exprObjectFields.every(
    (field) =>
      staticRootStyleNumberExpression(field.expr, constants) !== null ||
      staticRootStyleStringValue(field.expr, constants) !== null,
  )
}

function staticRootStyleNumberExpression(expr: string, constants: ConstantMap): string | null {
  const text = stripOuterParens(expr.trim())
  const literal = numericLiteralValue(text)
  if (literal !== null) return literal
  const ident = simpleIdentifierValue(text)
  const constant = ident ? constants.get(ident) : null
  return constant?.valueType === 'number' ? constant.value : null
}

function staticRootStyleStringValue(expr: string, constants: ConstantMap): string | null {
  const text = stripOuterParens(expr.trim())
  const literal = stringLiteralValue(text)
  if (literal !== null) return literal
  const ident = simpleIdentifierValue(text)
  const constant = ident ? constants.get(ident) : null
  return constant?.valueType === 'string' ? constant.value : null
}

function lowerRootEvent(
  varName: string,
  slot: GeaIrSlot,
  storeFields: StoreFieldPlan[] = [],
  storeMethods: StoreMethodPlan[] = [],
  fieldAccess?: FieldAccess,
  rootTag?: string,
  constants: ConstantMap = new Map(),
  disposerVar?: string,
): string[] | null {
  const attrName = slotAttrName(slot)
  const eventType = attrName ? eventTypeFromAttr(attrName) : null
  const handler = lowerEventHandler(slot.expr, eventType, storeFields, storeMethods, fieldAccess, constants)
  if (!eventType || !handler) return null
  const hoisted = hoistDirectRootEventStores(handler, varName, slot.index)

  // Document-level events have no element target. `rotary` (the encoder dial) is
  // always document-level. `keydown` is document-level only when we know it's
  // bound on a non-<input> element (an <input> keeps its per-node keydown
  // listener; when the tag is unknown — e.g. a nested child — we conservatively
  // keep keydown per-node). Document-level handlers register directly on
  // `document` (no body delegation, no containsNode/currentTarget bookkeeping)
  // and the runtime dispatches them off-tree.
  const isDocumentLevel =
    eventType === 'rotary' ||
    (eventType === 'keydown' && rootTag !== undefined && rootTag.toLowerCase() !== 'input')
  if (isDocumentLevel) {
    const docCaptures = [
      ...(hoisted.handler.some((line) => new RegExp(`\\b${varName}\\b`).test(line)) ? [varName] : []),
      ...hoisted.captures,
      ...(hoisted.handler.some((line) => /\bstore\b/.test(line)) ? ['store'] : []),
    ].join(', ')
    return [
      ...hoisted.setup.map((line) => `  ${line}`),
      `  gea::embedded::ui::Document::instance().addEventListener(${cppString(eventType)}, [${docCaptures}](gea::framework::events::PointerEvent &event) mutable {`,
      ...hoisted.handler.map((line) => `    ${line}`),
      '  });',
    ]
  }

  const captureList = [
    varName,
    ...hoisted.captures,
    ...(hoisted.handler.some((line) => /\bstore\b/.test(line)) ? ['store'] : []),
  ].join(', ')
  const listenerToken = `__gea_event_listener_${sanitizeCppIdentifier(varName)}_${slot.index}`
  const lines = [
    ...hoisted.setup.map((line) => `  ${line}`),
    `  auto ${listenerToken} = gea::embedded::ui::Document::instance().body().addEventListener(${cppString(eventType)}, [${captureList}](gea::framework::events::PointerEvent &event) mutable {`,
    `    if (!gea::embedded::ui::Tree::instance().containsNode(${varName}.id(), event.targetId)) return;`,
    '    const auto __gea_prev_current_target = event.currentTarget;',
    '    const auto __gea_prev_current_target_id = event.currentTargetId;',
    '    const auto __gea_prev_event_phase = event.eventPhase;',
    `    event.currentTarget = gea::framework::events::EventTarget(${varName}.id());`,
    `    event.currentTargetId = ${varName}.id();`,
    `    event.eventPhase = event.targetId == ${varName}.id() ? gea::framework::events::EventPhase::AtTarget : gea::framework::events::EventPhase::Bubbling;`,
    ...hoisted.handler.map((line) => `    ${line}`),
    '    event.currentTarget = __gea_prev_current_target;',
    '    event.currentTargetId = __gea_prev_current_target_id;',
    '    event.eventPhase = __gea_prev_event_phase;',
    '  });',
  ]
  if (disposerVar) {
    lines.push(`  if (${disposerVar}) {`)
    lines.push(`    ${disposerVar}->add([${listenerToken}]() -> void {`)
    lines.push(`      gea::embedded::ui::Document::instance().body().removeEventListener(${cppString(eventType)}, ${listenerToken});`)
    lines.push('    });')
    lines.push('  }')
  }
  return lines
}

function hoistDirectRootEventStores(
  handler: string[],
  varName: string,
  slotIndex: number,
): { captures: string[]; handler: string[]; setup: string[] } {
  const captures: string[] = []
  const setup: string[] = []
  const replacements = new Map<string, string>()
  // The hoisted capture lands at the enclosing mount-function scope alongside
  // every other event handler in the same component. `slotIndex` is reused
  // across elements (each element's event slots restart their numbering), so
  // it alone is not unique within the function — two handlers on different
  // elements can share a slotIndex and collide (redefinition). `varName` is
  // the per-element node variable (`__gea_node_N`/`root`), unique within the
  // function, so prefixing with it makes the capture name unique per emission
  // site while staying deterministic across builds.
  const scopePrefix = sanitizeCppIdentifier(varName)
  const rewritten = handler.map((line) => {
    const match = line.match(/^(\s*)if \(auto ([A-Za-z_][A-Za-z0-9_]*) = gea_cpp_value_as_shared_ptr<([^>]+)>\((.+)\)\) \{$/)
    if (match) {
      const [, indent, localName, storeClass, receiverExpr] = match
      const captureName = `__gea_event_store_${scopePrefix}_${slotIndex}_${captures.length}`
      captures.push(captureName)
      setup.push(`auto ${captureName} = gea_cpp_value_as_shared_ptr<${storeClass}>(${receiverExpr});`)
      replacements.set(localName, captureName)
      return `${indent}if (${captureName}) {`
    }
    let next = line
    for (const [from, to] of replacements) {
      next = next.replace(new RegExp(`\\b${from}\\b`, 'g'), to)
    }
    return next
  })
  return { captures, handler: rewritten, setup }
}

function lowerEventHandler(
  expr: string | undefined,
  eventType: string | null,
  storeFields: StoreFieldPlan[],
  storeMethods: StoreMethodPlan[],
  fieldAccess?: FieldAccess,
  constants: ConstantMap = new Map(),
): string[] | null {
  if (!expr || !eventType) return null
  const arrow = parseArrowFunction(expr)
  if (!arrow) return null

  const lines: string[] = []
  for (let index = 0; index < arrow.body.length; index += 1) {
    const lowered = lowerEventStatement(arrow.body[index], arrow.params, eventType, index, storeFields, storeMethods, fieldAccess, constants)
    if (!lowered) return null
    lines.push(...lowered)
  }
  return lines.length > 0 ? lines : null
}

function lowerEventStatement(
  statement: string,
  params: string[],
  eventType: string,
  statementIndex: number,
  storeFields: StoreFieldPlan[],
  storeMethods: StoreMethodPlan[],
  fieldAccess?: FieldAccess,
  constants: ConstantMap = new Map(),
): string[] | null {
  const guarded = statement.match(/^if\s*\(([\s\S]+?)\)\s*([\s\S]+)$/)
  if (guarded) {
    const condition = lowerEventCondition(guarded[1].trim(), params)
    const consequent = guarded[2].trim().replace(/;$/, '')
    const body = condition ? lowerEventStatement(consequent, params, eventType, statementIndex, storeFields, storeMethods, fieldAccess, constants) : null
    if (!condition || !body || body.length === 0) return null
    return [`if (${condition}) {`, ...body.map((line) => `  ${line}`), '}']
  }

  const freeCall = statement.match(/^([A-Za-z_$][A-Za-z0-9_$]*)\((.*)\)$/s)
  if (freeCall) {
    const [, fnName, rawArgs] = freeCall
    const args = splitTopLevelComma(rawArgs).filter((arg) => arg !== '')
    const loweredArgs: LoweredEventArg[] = []
    for (const arg of args) {
      const lowered = lowerEventArgument(arg, params, eventType, storeFields, constants)
      if (!lowered) return null
      loweredArgs.push(lowered)
    }
    return [`(void)fn_${sanitizeCppIdentifier(fnName)}(${loweredArgs.map((arg) => arg.direct).join(', ')});`]
  }

  const call = statement.match(/^([A-Za-z_$][A-Za-z0-9_$]*)\.([A-Za-z_$][A-Za-z0-9_$]*)\((.*)\)$/s)
  if (!call) return null

  const [, receiver, method, rawArgs] = call
  const args = splitTopLevelComma(rawArgs).filter((arg) => arg !== '')

  if (params.includes(receiver) && (method === 'preventDefault' || method === 'stopPropagation')) {
    if (args.length > 0) return null
    return [`event.${method}();`]
  }
  if (params.includes(receiver)) return null

  const loweredArgs: LoweredEventArg[] = []
  for (const arg of args) {
    const lowered = lowerEventArgument(arg, params, eventType, storeFields, constants)
    if (!lowered) return null
    loweredArgs.push(lowered)
  }

  const directMethod = storeMethodByReceiver(receiver, method, storeMethods)
  const directArgs = directMethod ? directCallArguments(directMethod, loweredArgs) : null
  if (directMethod && directArgs) {
    // TypedSelf self-store: `store` IS the typed instance — call directly, no
    // gea_cpp_value unbox.
    if (fieldAccess?.typed && receiver === 'this') {
      return [`(void)${fieldAccess.callOwnMethod(directMethod.methodName, directArgs)};`]
    }
    const receiverExpr = receiver === 'this' ? 'store' : `__gea_global_${sanitizeCppIdentifier(receiver)}()`
    // Typed global cells: Store singletons return shared_ptr; ordinary
    // exported singleton classes return references. Call both directly and
    // never box the receiver into gea_cpp_value.
    if (receiver !== 'this' && directMethod.globalAccess === 'reference') {
      return [`(void)${receiverExpr}.${directMethod.methodName}(${directArgs.join(', ')});`]
    }
    if (receiver !== 'this' && (directMethod.globalAccess === 'shared_ptr' || storeGlobalIsTyped(receiver, storeFields))) {
      return [`if (${receiverExpr}) (void)${receiverExpr}->${directMethod.methodName}(${directArgs.join(', ')});`]
    }
    const storeVar = `__gea_store_${statementIndex}`
    return [
      `if (auto ${storeVar} = gea_cpp_value_as_shared_ptr<${directMethod.storeClass}>(${receiverExpr})) {`,
      `  (void)${storeVar}->${directMethod.methodName}(${directArgs.join(', ')});`,
      `}`,
    ]
  }

  const callArgs = loweredArgs.map((arg) => arg.dynamic).join(', ')
  if ((receiver === 'Apps' || receiver === 'apps') && method === 'launch' && loweredArgs.length === 1) {
    return [`(void)gea::host::apps.launch(gea_cpp_to_string(${loweredArgs[0].dynamic}));`]
  }

  const methodVar = `__gea_method_${statementIndex}`
  // Resolve the actual receiver rather than blindly using the component's
  // `store` parameter. The previous implementation always looked the method
  // up on `store`, which silently worked only when the receiver happened to
  // match the inferred store (e.g. `launcher.tick()` in a component whose
  // refs already pulled in the launcher store). For multi-store components
  // — e.g. App's touch handlers calling `Settings.handleSwipe*()` while its
  // children refer to `launcher` — that ignored-receiver behaviour calls the
  // wrong store at runtime. Using `__gea_global_<receiver>()` resolves the
  // call to the real global, so components can import and call any number of
  // stores without being constrained to a single "event store". `this.method`
  // (class-component self calls) still routes through `store` since that's
  // the legacy contract the class lowering relies on.
  // TypedSelf self-store: `store` is the typed instance (no record_get_literal).
  if (fieldAccess?.typed && receiver === 'this') {
    return [`(void)${fieldAccess.callOwnMethod(method, loweredArgs.map((arg) => arg.direct))};`]
  }
  const receiverExpr = receiver === 'this' ? 'store' : `__gea_global_${sanitizeCppIdentifier(receiver)}()`
  // gea_cpp_key boxes a typed global cell for this dynamic lookup (identity
  // for boxed receivers).
  return [
    `auto ${methodVar} = gea_cpp_key(gea_cpp_key(${receiverExpr}).record_get_literal(${cppString(method)}));`,
    `if (gea::runtime::coerce::to_boolean(${methodVar})) (void)${methodVar}(${callArgs});`,
  ]
}

function lowerEventCondition(expr: string, params: string[]): string | null {
  const keyCode = expr.match(/^([A-Za-z_$][A-Za-z0-9_$]*)\.(keyCode|which|charCode)\s*(===|==|!==|!=)\s*(-?(?:\d+|\d+\.\d+|\.\d+))$/)
  if (keyCode && params.includes(keyCode[1])) {
    const op = keyCode[3] === '===' ? '==' : keyCode[3] === '!==' ? '!=' : keyCode[3]
    return `event.keyCode ${op} ${keyCode[4]}`
  }
  return null
}

interface LoweredEventArg {
  dynamic: string
  direct: string
  valueType: 'number' | 'string' | 'boolean' | 'any'
}

function eventArg(dynamic: string, direct: string, valueType: LoweredEventArg['valueType']): LoweredEventArg {
  return { dynamic, direct, valueType }
}

function lowerEventArgument(
  arg: string,
  params: string[],
  eventType: string,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap = new Map(),
): LoweredEventArg | null {
  const number = numericLiteralValue(arg)
  if (number !== null) {
    const direct = `static_cast<double>(${number})`
    return eventArg(`gea_cpp_value(${direct})`, direct, 'number')
  }

  const string = stringLiteralValue(arg)
  if (string !== null) {
    const direct = `std::string(${cppString(string)})`
    return eventArg(`gea_cpp_value(${direct})`, direct, 'string')
  }

  const identifier = simpleIdentifierValue(arg)
  const constant = identifier ? constantExpression(identifier, constants) : null
  if (constant?.shape.kind === 'literal') {
    if (constant.shape.valueType === 'number') {
      return eventArg(`gea_cpp_value(${constant.expr})`, constant.expr, 'number')
    }
    if (constant.shape.valueType === 'string') {
      return eventArg(`gea_cpp_value(${constant.expr})`, constant.expr, 'string')
    }
    if (constant.shape.valueType === 'boolean') {
      return eventArg(`gea_cpp_value(${constant.expr})`, constant.expr, 'boolean')
    }
  }

  if (params.length === 1 && arg === params[0] && eventType === 'click') {
    const direct = 'static_cast<double>(event.pressValue >= 0 ? event.pressValue : event.pressId)'
    return eventArg(`gea_cpp_value(${direct})`, direct, 'number')
  }
  if (params.length === 1 && arg === params[0]) return eventArg('gea_ir::pointerEventValue(event)', 'gea_ir::pointerEventValue(event)', 'any')

  // Native scalar fields of the framework PointerEvent. `pointerId` joins the
  // coordinate set so multi-touch handlers can route by finger
  // (`e => store.move(e.pointerId, e.clientX, e.clientY)`) and lower to a native
  // `store->move(double, double, double)` call with no gea_cpp_value boxing.
  const coord = arg.match(/^([A-Za-z_$][A-Za-z0-9_$]*)\.(x|y|clientX|clientY|pageX|pageY|screenX|screenY|pointerId)$/)
  if (coord && params.includes(coord[1])) {
    const direct = `static_cast<double>(event.${coord[2]})`
    return eventArg(`gea_cpp_value(${direct})`, direct, 'number')
  }

  const firstTouchCoord = arg.match(
    /^([A-Za-z_$][A-Za-z0-9_$]*)\.(touches|targetTouches|changedTouches)\[0\]\.(clientX|clientY|pageX|pageY|screenX|screenY)$/,
  )
  if (firstTouchCoord && params.includes(firstTouchCoord[1])) {
    const direct = `static_cast<double>(event.${firstTouchCoord[2]}[0].${firstTouchCoord[3]})`
    return eventArg(`gea_cpp_value(${direct})`, direct, 'number')
  }

  const attr = arg.match(/^([A-Za-z_$][A-Za-z0-9_$]*)\.(currentTarget|target)\.getAttribute\((['"])([^'"]+)\3\)$/)
  if (attr && params.includes(attr[1])) {
    const target = attr[2] === 'currentTarget' ? 'currentTarget' : 'target'
    const direct = `std::string(event.${target}.getAttribute(${cppString(attr[4])}))`
    return eventArg(`gea_cpp_value(${direct})`, direct, 'string')
  }

  const dataset = arg.match(/^([A-Za-z_$][A-Za-z0-9_$]*)\.(currentTarget|target)\.dataset\.([A-Za-z_$][A-Za-z0-9_$]*)$/)
  if (dataset && params.includes(dataset[1])) {
    const target = dataset[2] === 'currentTarget' ? 'currentTarget' : 'target'
    const direct = `std::string(event.${target}.dataset(${cppString(dataset[3])}))`
    return eventArg(`gea_cpp_value(${direct})`, direct, 'string')
  }

  // `event.currentTarget.value` (or `event.target.value`) — read the
  // input element's current text. The native side mirrors NSTextField's
  // stringValue into the node's `value` attribute on every change (see
  // GeaInputField::controlTextDidChange in targets/macos), so we lower
  // this to the same getAttribute path the dataset / getAttribute cases
  // already use.
  const inputValue = arg.match(/^([A-Za-z_$][A-Za-z0-9_$]*)\.(currentTarget|target)\.value$/)
  if (inputValue && params.includes(inputValue[1])) {
    const target = inputValue[2] === 'currentTarget' ? 'currentTarget' : 'target'
    const direct = `std::string(event.${target}.getAttribute("value"))`
    return eventArg(`gea_cpp_value(${direct})`, direct, 'string')
  }

  // `event.keyCode` / `event.which` (and `.charCode` for legacy callers)
  // — keyboard events. The framework's PointerEvent carries the integer
  // code in `event.keyCode`; both DOM aliases collapse to the same field.
  const keyCode = arg.match(/^([A-Za-z_$][A-Za-z0-9_$]*)\.(keyCode|which|charCode)$/)
  if (keyCode && params.includes(keyCode[1])) {
    const direct = 'static_cast<double>(event.keyCode)'
    return eventArg(`gea_cpp_value(${direct})`, direct, 'number')
  }

  const delta = arg.match(/^([A-Za-z_$][A-Za-z0-9_$]*)\.delta$/)
  if (delta && params.includes(delta[1])) {
    const direct = 'static_cast<double>(event.delta)'
    return eventArg(`gea_cpp_value(${direct})`, direct, 'number')
  }

  const storeField = arg.match(/^([A-Za-z_$][A-Za-z0-9_$]*)\.([A-Za-z_$][A-Za-z0-9_$]*)$/)
  if (storeField) {
    const field = storeFieldByReceiver(storeField[1], storeField[2], storeFields)
    if (field?.readerName) {
      const direct = `${field.readerName}(__gea_global_${sanitizeCppIdentifier(storeField[1])}())`
      return eventArg(`gea_cpp_key(${direct})`, direct, shapeValueType(shapeForStoreFieldPlan(field)))
    }
  }

  return null
}

function directCallArguments(method: StoreMethodPlan, args: LoweredEventArg[]): string[] | null {
  if (method.params.length !== args.length) return null
  const out: string[] = []
  for (let index = 0; index < args.length; index += 1) {
    const paramType = method.params[index]?.valueType
    if (!paramType) return null
    const arg = args[index]
    if (arg.valueType === paramType) {
      out.push(arg.direct)
      continue
    }
    if (paramType === 'number' && arg.valueType === 'any') {
      out.push(`gea::runtime::coerce::to_number(${arg.dynamic})`)
      continue
    }
    if (paramType === 'boolean' && arg.valueType === 'any') {
      out.push(`gea::runtime::coerce::to_boolean(${arg.dynamic})`)
      continue
    }
    if (paramType === 'string' && arg.valueType === 'any') {
      out.push(`gea_cpp_to_string(${arg.dynamic})`)
      continue
    }
    return null
  }
  return out
}

function storeFieldByReceiver(receiver: string, name: string, storeFields: StoreFieldPlan[]): StoreFieldPlan | null {
  const fieldName = sanitizeCppIdentifier(name)
  const matches = storeFields.filter((field) => field.fieldName === fieldName)
  if (matches.length === 1) return matches[0]
  const receiverName = sanitizeCppIdentifier(receiver)
  const byReceiver = matches.filter((field) => field.storeGlobalName === receiverName)
  return byReceiver.length === 1 ? byReceiver[0] : null
}

function storeMethodByReceiver(receiver: string, name: string, storeMethods: StoreMethodPlan[]): StoreMethodPlan | null {
  const methodName = sanitizeCppIdentifier(name)
  const receiverName = sanitizeCppIdentifier(receiver)
  const matches = storeMethods.filter((method) => method.methodName === methodName)
  const byReceiver = matches.filter((method) => method.storeGlobalName === receiverName)
  if (byReceiver.length === 1) return byReceiver[0]
  if (receiverName !== 'this' && matches.some((method) => !!method.storeGlobalName)) return null
  return matches.length === 1 ? matches[0] : null
}

function shapeValueType(shape: GeaIrStoreValueShape | null | undefined): LoweredEventArg['valueType'] {
  if (shape?.kind !== 'literal') return 'any'
  return shape.valueType === 'null' ? 'any' : shape.valueType
}

function parseArrowFunction(expr: string): { params: string[]; body: string[] } | null {
  const text = expr.trim()
  const arrow = text.indexOf('=>')
  if (arrow < 0) return null
  const rawParams = text.slice(0, arrow).trim()
  let body = text.slice(arrow + 2).trim()
  let statements: string[]
  if (body.startsWith('{') && body.endsWith('}')) {
    const inner = body.slice(1, -1).trim()
    const returnMatch = inner.match(/^return\s+(.+?);?$/s)
    statements = returnMatch
      ? [returnMatch[1].trim()]
      : splitTopLevelSemicolon(inner).map((statement) => statement.trim()).filter(Boolean)
  } else {
    statements = [body.replace(/;$/, '').trim()].filter(Boolean)
  }
  const params = rawParams.startsWith('(')
    ? rawParams.slice(1, -1).split(',').map((part) => part.trim()).filter(Boolean)
    : rawParams.length === 0
      ? []
      : [rawParams]
  if (params.some((param) => !/^[A-Za-z_$][A-Za-z0-9_$]*$/.test(param))) return null
  return { params, body: statements }
}

export function slotAttrName(slot: GeaIrSlot): string | null {
  const payload = slot.payload
  if (!payload || typeof payload !== 'object' || !('attrName' in payload)) return null
  const attrName = (payload as { attrName?: unknown }).attrName
  return typeof attrName === 'string' ? attrName : null
}

function eventTypeFromAttr(attrName: string): string {
  return attrName.startsWith('on') && attrName.length > 2 ? attrName.slice(2).toLowerCase() : attrName.toLowerCase()
}

function attributeValueExpression(expr: string, constants: ConstantMap = new Map()): string | null {
  const string = stringLiteralValue(expr)
  if (string !== null) return cppString(string)
  const number = numericLiteralValue(expr)
  if (number !== null) return `std::to_string(${number}).c_str()`
  if (expr === 'true' || expr === 'false') return cppString(expr)
  // Runtime numeric constants (UPPER_CASE module-level identifiers) are
  // resolved at template-setup time — they're set during module init, before
  // mount() runs, and never reassigned. Stringify them imperatively into
  // setAttribute rather than letting the slot fall out to a reactive
  // emission. Without this, `<canvas width={DISPLAY_W}>` and
  // `<virtual-list item-count={ITEM_COUNT}>` would force the entire
  // component down the closure-codegen reactiveAttr path whose initial
  // apply never fires in the C++ runtime.
  const ident = simpleIdentifierValue(expr)
  if (ident && isRuntimeNumericConstantIdentifier(ident)) {
    // Prefer folding the constant to its literal value (mirrors the style
    // path's `constantExpression` resolution). A CROSS-MODULE imported
    // `const DISPLAY_W = 410` is inlined-and-dropped by the bundler, so no C++
    // global `DISPLAY_W` survives to reference — emitting the bare identifier
    // then fails to compile ("use of undeclared identifier"). The IR constants
    // map still carries its value, so fold it. Fall back to the bare identifier
    // for constants NOT in the map (same-module constants emitted as real C++
    // globals), preserving the documented module-init behavior.
    const constant = constantExpression(ident, constants)
    if (constant && constant.shape.kind === 'literal' && constant.shape.valueType === 'number') {
      return `std::to_string(static_cast<long long>(${constant.expr})).c_str()`
    }
    return `std::to_string(static_cast<long long>(${ident})).c_str()`
  }
  return null
}

function simpleIdentifierValue(expr: string): string | null {
  const text = expr.trim()
  return /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(text) ? text : null
}

function parseStaticAttrs(rawTag: string): Record<string, string> {
  const attrs: Record<string, string> = {}
  const attrPattern = /\s([A-Za-z_:][A-Za-z0-9_:.-]*)(?:=(?:"([^"]*)"|'([^']*)'|([^ >]+)))?/g
  let match: RegExpExecArray | null
  while ((match = attrPattern.exec(rawTag))) {
    attrs[match[1]] = match[2] ?? match[3] ?? match[4] ?? 'true'
  }
  return attrs
}

function numericLiteralValue(expr: string): string | null {
  const text = expr.trim()
  return /^-?(?:\d+|\d+\.\d+|\.\d+)$/.test(text) ? text : null
}

export function lowerTextSlot(slot: GeaIrSlot, storeFields: StoreFieldPlan[]): { stateType: string; deps: string[]; fields: StoreFieldPlan[]; expr: string } | null {
  if (!slot.expr) return null
  const parts = slot.exprPath && slot.exprPath.length === 2 ? [slot.exprPath.join('.')] : splitTopLevelPlus(slot.expr)
  const stateTypes = new Set<string>()
  const deps: string[] = []
  const fields: StoreFieldPlan[] = []
  const expressions: string[] = []
  for (const part of parts) {
    const literal = stringLiteralValue(part)
    if (literal !== null) {
      expressions.push(`std::string(${cppString(literal)})`)
      continue
    }
    const match = part.match(/^[A-Za-z_$][A-Za-z0-9_$]*\.([A-Za-z_$][A-Za-z0-9_$]*)$/)
    if (!match) return null
    const lowered = lowerStoreFieldByName(match[1], storeFieldLocalName(match[1]), storeFields)
    if (!lowered) return null
    stateTypes.add(lowered.store.stateType)
    deps.push(lowered.store.fieldName)
    fields.push(lowered.store)
    expressions.push(textExpressionForStoreField(lowered.store, lowered.expr))
  }
  if (stateTypes.size !== 1 || expressions.length === 0) return null
  return { stateType: [...stateTypes][0], deps, fields: uniqueStoreFields(fields), expr: expressions.join(' + ') }
}

interface LoweredStoreStyleExpression {
  store: StoreFieldPlan
  expr: string
  shape?: GeaIrStoreValueShape | null
  styleUnit?: 'percent' | 'color' | 'keyword' | 'px'
}

function lowerStoreExpression(
  field: GeaIrExpressionObjectField,
  storeFields: StoreFieldPlan[],
): LoweredStoreStyleExpression | null {
  const fromPath = field.exprPath && field.exprPath.length === 2 ? field.exprPath[1] : null
  if (fromPath) return lowerStoreFieldByName(fromPath, storeFieldLocalName(fromPath), storeFields)

  const binary = field.expr.match(/^[A-Za-z_$][A-Za-z0-9_$]*\.([A-Za-z_$][A-Za-z0-9_$]*)\s*([+-])\s*(-?(?:\d+|\d+\.\d+|\.\d+))$/)
  if (!binary) return null
  const [, fieldName, op, amount] = binary
  return lowerStoreFieldByName(fieldName, `(${storeFieldLocalName(fieldName)} ${op} ${amount})`, storeFields)
}

function lowerStorePxLengthStyleExpression(
  field: GeaIrExpressionObjectField,
  propertyName: string,
  storeFields: StoreFieldPlan[],
): LoweredStoreStyleExpression | null {
  if (!isPxLengthStylePropertyName(propertyName) || !field.expr) return null
  const source = ts.createSourceFile('style.ts', `(${field.expr});`, ts.ScriptTarget.Latest, false)
  const stmt = source.statements[0]
  if (!stmt || !ts.isExpressionStatement(stmt)) return null
  const node = unwrapExpressionNode(stmt.expression)
  if (!ts.isTemplateExpression(node)) return null
  if (node.head.text !== '' || node.templateSpans.length !== 1) return null
  const span = node.templateSpans[0]
  if (span.literal.text !== 'px') return null
  const expr = unwrapExpressionNode(span.expression)
  if (!ts.isPropertyAccessExpression(expr)) return null
  const lowered = lowerStoreFieldByName(expr.name.text, storeFieldLocalName(expr.name.text), storeFields)
  return lowered ? { ...lowered, shape: { kind: 'literal', valueType: 'number' }, styleUnit: isBorderWidthStylePropertyName(propertyName) ? 'px' : undefined } : null
}

function lowerStorePercentLengthStyleExpression(
  field: GeaIrExpressionObjectField,
  propertyName: string,
  storeFields: StoreFieldPlan[],
): LoweredStoreStyleExpression | null {
  if (!stylePercentPropertyEnumForPropertyName(propertyName) || !field.expr) return null
  const source = ts.createSourceFile('style.ts', `(${field.expr});`, ts.ScriptTarget.Latest, false)
  const stmt = source.statements[0]
  if (!stmt || !ts.isExpressionStatement(stmt)) return null
  const node = unwrapExpressionNode(stmt.expression)
  if (!ts.isTemplateExpression(node)) return null
  if (node.head.text !== '' || node.templateSpans.length !== 1) return null
  const span = node.templateSpans[0]
  if (span.literal.text !== '%') return null
  const expr = unwrapExpressionNode(span.expression)
  if (!ts.isPropertyAccessExpression(expr)) return null
  const lowered = lowerStoreFieldByName(expr.name.text, storeFieldLocalName(expr.name.text), storeFields)
  return lowered ? { ...lowered, shape: { kind: 'literal', valueType: 'number' }, styleUnit: 'percent' } : null
}

function lowerStoreRotateStyleExpression(
  field: GeaIrExpressionObjectField,
  propertyName: string,
  storeFields: StoreFieldPlan[],
): LoweredStoreStyleExpression | null {
  if (!isRotateTransformPropertyName(propertyName) || !field.expr) return null
  const source = ts.createSourceFile('style.ts', `(${field.expr});`, ts.ScriptTarget.Latest, false)
  const stmt = source.statements[0]
  if (!stmt || !ts.isExpressionStatement(stmt)) return null
  const node = unwrapExpressionNode(stmt.expression)
  if (!ts.isTemplateExpression(node)) return null
  const degrees = rotateDegreesTemplateExpression(node, propertyName)
  if (!degrees) return null
  const expr = unwrapExpressionNode(degrees)
  if (!ts.isPropertyAccessExpression(expr)) return null
  const lowered = lowerStoreFieldByName(expr.name.text, storeFieldLocalName(expr.name.text), storeFields)
  return lowered ? { ...lowered, shape: { kind: 'literal', valueType: 'number' } } : null
}

function lowerStoreOpaqueColorStyleExpression(
  field: GeaIrExpressionObjectField,
  propertyName: string,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
): LoweredStoreStyleExpression | null {
  if (!styleOpaqueColorTargetForPropertyName(propertyName) || !field.expr) return null
  const source = ts.createSourceFile('style.ts', `(${field.expr});`, ts.ScriptTarget.Latest, false)
  const stmt = source.statements[0]
  if (!stmt || !ts.isExpressionStatement(stmt)) return null
  const node = unwrapExpressionNode(stmt.expression)
  if (!ts.isConditionalExpression(node)) return null
  const condition = unwrapExpressionNode(node.condition)
  if (!ts.isPropertyAccessExpression(condition)) return null
  const consequent = opaqueHexColorExpressionForNode(node.whenTrue, constants)
  const alternate = opaqueHexColorExpressionForNode(node.whenFalse, constants)
  if (!consequent || !alternate) return null
  const fieldName = condition.name.text
  const lowered = lowerStoreFieldByName(fieldName, storeFieldLocalName(fieldName), storeFields)
  if (!lowered) return null
  return {
    ...lowered,
    expr: `(${storeFieldAsBoolean(lowered.store, storeFieldLocalName(fieldName))} ? ${consequent} : ${alternate})`,
    shape: { kind: 'literal', valueType: 'number' },
    styleUnit: 'color',
  }
}

function lowerStoreKeywordStyleExpression(
  field: GeaIrExpressionObjectField,
  propertyName: string,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
): LoweredStoreStyleExpression | null {
  if (!styleKeywordPropertiesForPropertyName(propertyName) || !field.expr) return null
  const source = ts.createSourceFile('style.ts', `(${field.expr});`, ts.ScriptTarget.Latest, false)
  const stmt = source.statements[0]
  if (!stmt || !ts.isExpressionStatement(stmt)) return null
  const node = unwrapExpressionNode(stmt.expression)
  if (!ts.isConditionalExpression(node)) return null
  const condition = unwrapExpressionNode(node.condition)
  if (!ts.isPropertyAccessExpression(condition)) return null
  const consequent = keywordEntriesForNode(propertyName, node.whenTrue, constants)
  const alternate = keywordEntriesForNode(propertyName, node.whenFalse, constants)
  const consequentValue = keywordCommonValue(consequent)
  const alternateValue = keywordCommonValue(alternate)
  if (consequentValue === null || alternateValue === null || !keywordEntriesCompatible(consequent, alternate)) return null
  const fieldName = condition.name.text
  const lowered = lowerStoreFieldByName(fieldName, storeFieldLocalName(fieldName), storeFields)
  if (!lowered) return null
  return {
    ...lowered,
    expr: `(${storeFieldAsBoolean(lowered.store, storeFieldLocalName(fieldName))} ? ${consequentValue} : ${alternateValue})`,
    shape: { kind: 'literal', valueType: 'number' },
    styleUnit: 'keyword',
  }
}

function isRotateTransformPropertyName(propertyName: string): boolean {
  return propertyName === 'transform' || propertyName === 'rotate'
}

function rotateDegreesTemplateExpression(node: ts.TemplateExpression, propertyName: string): ts.Expression | null {
  if (node.templateSpans.length !== 1) return null
  const span = node.templateSpans[0]
  if (propertyName === 'rotate') {
    if (node.head.text !== '' || span.literal.text.trim() !== 'deg') return null
    return span.expression
  }
  if (propertyName === 'transform') {
    if (node.head.text.trim() !== 'rotate(' || span.literal.text.trim() !== 'deg)') return null
    return span.expression
  }
  return null
}

function lowerStoreFieldByName(
  name: string,
  expr: string,
  storeFields: StoreFieldPlan[],
): LoweredStoreStyleExpression | null {
  const sanitized = sanitizeCppIdentifier(name)
  const matches = storeFields.filter((candidate) => candidate.fieldName === sanitized)
  return matches.length === 1 ? { store: matches[0], expr } : null
}

export function storeFieldLocalName(fieldName: string): string {
  return `__gea_${sanitizeCppIdentifier(fieldName)}`
}

function uniqueStoreFields(fields: StoreFieldPlan[]): StoreFieldPlan[] {
  const seen = new Set<string>()
  const uniqueFields: StoreFieldPlan[] = []
  for (const field of fields) {
    const key = `${field.storeClass}.${field.fieldName}`
    if (seen.has(key)) continue
    seen.add(key)
    uniqueFields.push(field)
  }
  return uniqueFields
}

function textExpressionForStoreField(store: StoreFieldPlan, expr: string): string {
  const shape = shapeForStoreFieldPlan(store)
  if (shape?.kind === 'literal' && shape.valueType === 'string') return expr
  return `gea_cpp_to_string(${expr})`
}

function storeFieldAsBoolean(store: StoreFieldPlan, expr: string): string {
  const shape = shapeForStoreFieldPlan(store)
  if (shape?.kind === 'literal' && shape.valueType === 'boolean') return expr
  if (shape?.kind === 'literal' && shape.valueType === 'number') return `(${expr} != static_cast<double>(0))`
  if (shape?.kind === 'literal' && shape.valueType === 'string') return `(!${expr}.empty())`
  return `gea::runtime::coerce::to_boolean(gea_cpp_key(${expr}))`
}

function directNumberStyleApplyLine(node: string, propertyName: string, value: string): string | null {
  if (isBorderWidthStylePropertyName(propertyName)) {
    const declaration = styleDeclarationEnumForPropertyName(propertyName)
    return `gea::embedded::ui::StyleSheet::instance().applyNumberProperty(${node}, gea::embedded::ui::StyleDeclaration::${declaration}, static_cast<double>(${value}));`
  }
  const rawNumber = rawNumberExpression(value)
  const properties = styleRawNumberPropertyEnumsForPropertyName(propertyName)
  if (properties) {
    return properties.map((property) => `${node}.style().set(gea::embedded::ui::Property::${property}, ${rawNumber});`).join(' ')
  }
  if (propertyName === 'opacity') return `${node}.style().opacity(${opacityNumberExpression(value)});`
  if (propertyName === 'flex') {
    return `${node}.style().set(gea::embedded::ui::Property::Flex, ${rawNumber}); ${node}.style().set(gea::embedded::ui::Property::FlexBasis, gea::embedded::ui::kUnset);`
  }
  if (propertyName === 'translate-x') return `${node}.style().translateX(${rawNumber});`
  if (propertyName === 'translate-y') return `${node}.style().translateY(${rawNumber});`
  if (propertyName === 'scale') return `${node}.style().cssScale(static_cast<double>(${value}));`
  if (propertyName === 'rotate') return `${node}.style().cssRotateDegrees(static_cast<double>(${value}));`
  if (propertyName === 'transform') return `${node}.style().rotateDegrees(static_cast<double>(${value}));`
  return null
}

function rawNumberExpression(value: string): string {
  return `([](double __v) -> int { return std::isfinite(__v) ? static_cast<int>(std::round(__v)) : 0; })(static_cast<double>(${value}))`
}

function opacityNumberExpression(value: string): string {
  return `([](double __v) -> int { if (!std::isfinite(__v)) return 0; if (__v <= 1.0) __v *= 255.0; if (__v < 0.0) __v = 0.0; if (__v > 255.0) __v = 255.0; return static_cast<int>(__v + 0.5); })(static_cast<double>(${value}))`
}

function directStringStyleApplyLines(node: string, propertyName: string, rawValue: string): string[] | null {
  const color = opaqueCssHexColor(rawValue)
  const colorLine = color ? directColorStyleApplyLine(node, propertyName, nativeStyleColorExpression(color)) : null
  if (colorLine) return [colorLine]
  const keyword = styleKeywordPropertyValuesForPropertyName(propertyName, rawValue)
  return keyword
    ? keyword.map(({ property, value }) => `${node}.style().set(gea::embedded::ui::Property::${property}, ${value});`)
    : null
}

function directKeywordStyleApplyLine(node: string, propertyName: string, value: string): string | null {
  const properties = styleKeywordPropertiesForPropertyName(propertyName)
  return properties
    ? properties.map((property) => `${node}.style().set(gea::embedded::ui::Property::${property}, static_cast<int>(${value}));`).join(' ')
    : null
}

function keywordEntriesForNode(
  propertyName: string,
  node: ts.Expression,
  constants: ConstantMap = new Map(),
): StylePropertyValue[] | null {
  const literal = staticStringValueForNode(node, constants)
  return literal === null ? null : styleKeywordPropertyValuesForPropertyName(propertyName, literal)
}

function keywordEntriesCompatible(a: StylePropertyValue[] | null, b: StylePropertyValue[] | null): boolean {
  if (!a || !b || a.length !== b.length) return false
  for (let i = 0; i < a.length; i += 1) {
    if (a[i].property !== b[i].property) return false
  }
  return true
}

function keywordCommonValue(entries: StylePropertyValue[] | null): number | null {
  if (!entries || entries.length === 0) return null
  const first = entries[0].value
  return entries.every((entry) => entry.value === first) ? first : null
}

function directColorStyleApplyLine(node: string, propertyName: string, value: string): string | null {
  const target = styleOpaqueColorTargetForPropertyName(propertyName)
  if (target === 'background') {
    const resetImage = propertyName === 'background' ? `${node}.style().set(gea::embedded::ui::Property::BackgroundImage, -1); ${node}.style().set(gea::embedded::ui::Property::BackgroundClip, 0); ` : ''
    return `${resetImage}${node}.style().backgroundColor(static_cast<int>(${value}));`
  }
  if (target === 'color') return `${node}.style().color(static_cast<int>(${value}));`
  if (target === 'active-background') {
    return `${node}.style().set(gea::embedded::ui::Property::ActiveBackgroundColor, static_cast<int>(${value})); ${node}.style().set(gea::embedded::ui::Property::HasActiveBackground, 1);`
  }
  const borderProperty = opaqueColorBorderProperty(target)
  if (borderProperty) return `${node}.style().set(gea::embedded::ui::Property::${borderProperty}, static_cast<int>(${value}));`
  return null
}

function opaqueColorBorderProperty(target: ReturnType<typeof styleOpaqueColorTargetForPropertyName>): string | null {
  if (target === 'border-color') return 'BorderColor'
  if (target === 'border-top-color') return 'BorderTopColor'
  if (target === 'border-right-color') return 'BorderRightColor'
  if (target === 'border-bottom-color') return 'BorderBottomColor'
  if (target === 'border-left-color') return 'BorderLeftColor'
  return null
}

function stylePropertyArgument(propertyName: string): string {
  const declaration = styleDeclarationEnumForPropertyName(propertyName)
  return declaration ? `gea::embedded::ui::StyleDeclaration::${declaration}` : cppString(propertyName)
}

function styleApplyLines(
  node: string,
  propertyName: string,
  value: string,
  shape: GeaIrStoreValueShape | null,
  styleUnit?: 'percent' | 'color' | 'keyword' | 'px',
): string[] {
  const property = stylePropertyArgument(propertyName)
  if (styleUnit === 'px') return [`gea::embedded::ui::StyleSheet::instance().applyPixelLengthProperty(${node}, ${property}, static_cast<double>(${value}));`]
  if (shape?.kind === 'literal' && shape.valueType === 'number') {
    const direct = styleUnit === 'percent'
      ? directPercentStyleApplyLine(node, propertyName, value)
      : styleUnit === 'color'
        ? directColorStyleApplyLine(node, propertyName, value)
        : styleUnit === 'keyword'
          ? directKeywordStyleApplyLine(node, propertyName, value)
          : directNumberStyleApplyLine(node, propertyName, value)
    return [direct ?? `gea::embedded::ui::StyleSheet::instance().applyNumberProperty(${node}, ${property}, static_cast<double>(${value}));`]
  }
  if (shape?.kind === 'literal' && shape.valueType === 'string') {
    return [`gea::embedded::ui::StyleSheet::instance().applyProperty(${node}, ${property}, ${value});`]
  }
  if (shape?.kind === 'literal' && shape.valueType === 'boolean') {
    return [`gea::embedded::ui::StyleSheet::instance().applyProperty(${node}, ${property}, gea_cpp_to_string(${value}));`]
  }
  return [
    `gea::embedded::ui::StyleSheet::instance().applyProperty(${node}, ${property}, gea_cpp_to_string(gea_cpp_key(${value})));`,
  ]
}

function directPercentStyleApplyLine(node: string, propertyName: string, value: string): string | null {
  const property = stylePercentPropertyEnumForPropertyName(propertyName)
  return property
    ? `${node}.style().set(gea::embedded::ui::Property::${property}, static_cast<int>(std::round(static_cast<double>(${value}) * 10.0)));`
    : null
}

function valueShapeForStoreField(field: GeaIrStoreField | undefined): GeaIrStoreValueShape | null {
  if (!field) return null
  if (field.shape) return field.shape
  return shapeFromInitializer(field.initializer)
}

function shapeForStoreFieldPlan(field: StoreFieldPlan): GeaIrStoreValueShape | null {
  return field.shape ?? shapeFromPrimitiveFieldType(field.fieldType)
}

function shapeFromPrimitiveFieldType(fieldType: string): GeaIrStoreValueShape | null {
  const type = fieldType.trim()
  if (type === 'double' || type === 'gea_number') return { kind: 'literal', valueType: 'number' }
  if (type === 'std::string') return { kind: 'literal', valueType: 'string' }
  if (type === 'bool') return { kind: 'literal', valueType: 'boolean' }
  return null
}

function shapeFromInitializer(initializer: string | undefined): GeaIrStoreValueShape | null {
  if (!initializer) return null
  const text = initializer.trim()
  if (/^-?(?:\d+|\d+\.\d+|\.\d+)$/.test(text)) return { kind: 'literal', valueType: 'number' }
  if (text === 'true' || text === 'false') return { kind: 'literal', valueType: 'boolean' }
  if (text === 'null') return { kind: 'literal', valueType: 'null' }
  if ((text.startsWith("'") && text.endsWith("'")) || (text.startsWith('"') && text.endsWith('"'))) {
    return { kind: 'literal', valueType: 'string' }
  }
  return null
}

function splitTopLevelPlus(expr: string): string[] {
  const parts: string[] = []
  let start = 0
  let quote: string | null = null
  let escaped = false
  for (let index = 0; index < expr.length; index += 1) {
    const char = expr[index]
    if (quote) {
      if (escaped) escaped = false
      else if (char === '\\') escaped = true
      else if (char === quote) quote = null
      continue
    }
    if (char === '"' || char === "'") quote = char
    else if (char === '+') {
      parts.push(expr.slice(start, index).trim())
      start = index + 1
    }
  }
  parts.push(expr.slice(start).trim())
  return parts.filter(Boolean)
}

function splitTopLevelComma(expr: string): string[] {
  const parts: string[] = []
  let start = 0
  let quote: string | null = null
  let escaped = false
  let depth = 0
  for (let index = 0; index < expr.length; index += 1) {
    const char = expr[index]
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
    if (char === '(' || char === '[' || char === '{') depth += 1
    else if (char === ')' || char === ']' || char === '}') depth -= 1
    else if (char === ',' && depth === 0) {
      parts.push(expr.slice(start, index).trim())
      start = index + 1
    }
  }
  parts.push(expr.slice(start).trim())
  return parts
}

function findTopLevelOperator(expr: string, operator: string): number {
  let quote: string | null = null
  let escaped = false
  let depth = 0
  for (let index = 0; index < expr.length; index += 1) {
    const char = expr[index]
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
    if (char === '(' || char === '[' || char === '{') depth += 1
    else if (char === ')' || char === ']' || char === '}') depth -= 1
    else if (depth === 0 && expr.startsWith(operator, index)) return index
  }
  return -1
}

function splitTopLevelSemicolon(expr: string): string[] {
  const parts: string[] = []
  let start = 0
  let quote: string | null = null
  let escaped = false
  let depth = 0
  for (let index = 0; index < expr.length; index += 1) {
    const char = expr[index]
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
    if (char === '(' || char === '[' || char === '{') depth += 1
    else if (char === ')' || char === ']' || char === '}') depth -= 1
    else if (char === ';' && depth === 0) {
      parts.push(expr.slice(start, index).trim())
      start = index + 1
    }
  }
  parts.push(expr.slice(start).trim())
  return parts
}

function stripOuterParens(expr: string): string {
  let text = expr.trim()
  while (text.startsWith('(') && text.endsWith(')') && matchingOuterParens(text)) {
    text = text.slice(1, -1).trim()
  }
  return text
}

function matchingOuterParens(expr: string): boolean {
  let depth = 0
  let quote: string | null = null
  let escaped = false
  for (let index = 0; index < expr.length; index += 1) {
    const char = expr[index]
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
    if (char === '(') depth += 1
    else if (char === ')') {
      depth -= 1
      if (depth === 0 && index < expr.length - 1) return false
    }
  }
  return depth === 0
}

function objectKeyName(expr: string): string | null {
  const string = stringLiteralValue(expr)
  if (string !== null) return string
  return /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(expr) ? expr : null
}

function uniqueItemFieldNames(fields: string[]): string[] {
  return [...new Set(fields)]
}

function uniqueStrings(values: string[]): string[] {
  return [...new Set(values)]
}

export function stringLiteralValue(expr: string): string | null {
  const quote = expr[0]
  if ((quote !== '"' && quote !== "'") || expr[expr.length - 1] !== quote) return null
  return expr
    .slice(1, -1)
    .replace(/\\n/g, '\n')
    .replace(/\\t/g, '\t')
    .replace(/\\'/g, "'")
    .replace(/\\"/g, '"')
    .replace(/\\\\/g, '\\')
}

function opaqueHexColorExpressionForNode(node: ts.Expression, constants: ConstantMap = new Map()): string | null {
  const literal = staticStringValueForNode(node, constants)
  const color = literal === null ? null : opaqueCssHexColor(literal)
  return color ? nativeStyleColorExpression(color) : null
}

function staticStringValueForNode(node: ts.Expression, constants: ConstantMap): string | null {
  const expr = unwrapExpressionNode(node)
  if (ts.isStringLiteral(expr) || ts.isNoSubstitutionTemplateLiteral(expr)) return expr.text
  if (ts.isIdentifier(expr)) {
    const constant = constants.get(expr.text)
    return constant?.valueType === 'string' ? constant.value : null
  }
  return null
}

function keyedListKeyField(
  payload: GeaIrKeyedListPayload,
  itemName: string,
  field: StoreArrayFieldPlan,
): GeaIrStoreField | null {
  const explicit = explicitKeyFieldName(payload, itemName)
  if (explicit) return field.itemFields.find((candidate) => sanitizeCppIdentifier(candidate.name) === explicit) ?? null
  return field.itemFields.find((candidate) => sanitizeCppIdentifier(candidate.name) === 'id') ?? null
}

function explicitKeyFieldName(payload: GeaIrKeyedListPayload, itemName: string): string | null {
  const keyPath = payload.keyPath
  if (Array.isArray(keyPath) && keyPath.length === 2 && keyPath[0] === itemName && typeof keyPath[1] === 'string') {
    return sanitizeCppIdentifier(keyPath[1])
  }

  for (const name of ['keyProp', 'keyField']) {
    const value = payload[name]
    if (typeof value === 'string' && /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(value)) return sanitizeCppIdentifier(value)
  }

  for (const name of ['keyExpr', 'key']) {
    const value = payload[name]
    if (typeof value !== 'string') continue
    const match = value.match(new RegExp(`^${itemName.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}\\.([A-Za-z_$][A-Za-z0-9_$]*)$`))
    if (match) return sanitizeCppIdentifier(match[1])
  }

  return null
}
