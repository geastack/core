import type { StoreArrayFieldPlan, StoreFieldPlan, StoreMethodPlan } from './cpp-stores.js'
import {
  attributeLines,
  attributeSlotLines,
  childForMount,
  constantExpression,
  eventSetupLines,
  fieldNameForSlot,
  imageSrcAssetIdExpression,
  lowerRowSlots,
  isRootSetupSlot,
  mountSlotStyleSlot,
  mountSlotTag,
  objectArrayConstant,
  rootSetupLines,
  slotAttrName,
  storeFieldLocalName,
  type ConstantMap,
} from './cpp-mounted-lowering.js'
import type {
  GeaIrComponent,
  GeaIrConstantObjectArrayItem,
  GeaIrConstantObjectField,
  GeaIrExpressionObjectField,
  GeaIrSlot,
  GeaIrStoreValueShape,
  GeaIrTemplate,
} from './types.js'
import {
  cppString,
  isPxLengthStylePropertyName,
  isBorderWidthStylePropertyName,
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
import { BoxedStoreFieldAccess, type FieldAccess } from './cpp-field-access.js'
import { arrayRevFieldName, reactiveArrayFieldInfo, type ReactiveArrayFieldInfo } from './cpp-reactive-component.js'
import { storeHubEmissionEnabled } from './cpp-store-hub.js'
import { geaHostShims } from './host-shims.js'

// Receivers that are host facades / JS globals, NOT app store singletons. A
// bare `receiver.field` read on one of these must NOT be mistaken for a global
// store field (see `dynamicGlobalStoreFieldRef`): `display.width`,
// `Math.PI`, `window.x`, etc. route through their own host/intrinsic paths.
const NON_STORE_RECEIVERS = new Set<string>([
  ...Object.keys(geaHostShims.hostNamespaces ?? {}),
  ...Object.keys(geaHostShims.hostGlobalObjects ?? {}),
  'Math',
  'JSON',
  'Date',
  'Object',
  'Array',
  'Number',
  'String',
  'Boolean',
  'console',
  'document',
  'window',
  'props',
  'this',
])

// Default code-emit strategy: today's boxed `gea_cpp_value` store reads. The
// renderer threads a `FieldAccess` (this or a TypedSelf) so a self-store
// ReactiveComponent emits typed `self->field` reads through the SAME renderer.
// Existing (non-reactive) callers pass nothing → boxed → byte-identical output.
const BOXED_FIELD_ACCESS = new BoxedStoreFieldAccess()

export interface TemplateElement {
  kind: 'element'
  tag: string
  className?: string
  attrs?: Record<string, string>
  children: TemplateChild[]
}

export type TemplateChild = TemplateElement | TemplateText | TemplateSlot

export interface TemplateText {
  kind: 'text'
  text: string
}

export interface TemplateSlot {
  kind: 'slot'
  index: number
}

interface PropBinding {
  expr: string
  parentProps: Map<string, PropBinding>
  // When set, this binding is JSX content forwarded from the parent (typically
  // `<Wrapper>{children}</Wrapper>` syntax becoming `props.children` on the
  // wrapper). The wrapper's `props.children` text slot renders this sub-tree
  // inline at its position instead of attempting to lower it as a string.
  childrenTemplate?: GeaIrTemplate
}

type StoreFieldRef = StoreFieldPlan & { receiverName?: string }

interface LoweredExpression {
  kind: 'string' | 'number' | 'boolean' | 'value'
  expr: string
  deps: string[]
  fields: StoreFieldRef[]
  styleUnit?: 'percent' | 'color' | 'keyword' | 'px'
}

interface LoweredClassObject {
  lowered: LoweredExpression
  entries: Array<{ className: string; condition: LoweredExpression }>
}

function activeDisposerVar(args: { disposerVar?: string }): string {
  return args.disposerVar ?? 'disposer'
}

export function templateMountedRenderer(
  component: GeaIrComponent,
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  canMount: (component: GeaIrComponent) => boolean,
  storeMethods: StoreMethodPlan[] = [],
  fieldAccess: FieldAccess = BOXED_FIELD_ACCESS,
  storeArrayFields: StoreArrayFieldPlan[] = [],
): string[] | null {
  const root = parseTemplateRoot(component.template.html)
  const transparentImage = root ? transparentDisplayContentsImageMount(component, root) : null
  if (transparentImage) {
    const lines: string[] = [
      `inline gea::embedded::ui::NodeHandle mount_${sanitizeCppIdentifier(component.exportName)}(${fieldAccess.paramDecl}, const std::shared_ptr<NativeDisposer> &disposer) {`,
      '  auto root = gea::embedded::ui::Document::instance().createImage();',
      ...attributeLines('root', { tag: 'img' }),
    ]
    if (!emitIntrinsicImageMount(lines, 'root', transparentImage, storeFields, constants, new Map(), 'disposer')) return null
    lines.push('  return root;')
    lines.push('}')
    lines.push('')
    return lines
  }

  // Drill through any transparent wrapper chain. If the wrapped element has no
  // props we can keep the cheap delegation; otherwise we emit the wrapped
  // element's template inline with the composed bindings so `<View class="x">…
  // </View>` compiles to the same code as `<div class="x">…</div>`.
  const resolved = root ? resolveTransparentWrappers(component, components, new Map(), canMount) : null
  if (resolved && resolved.component !== component) {
    if (resolved.propBindings.size === 0) {
      const delegateStoreArg = fieldAccess.childMountStoreArg(resolved.component)
      if (delegateStoreArg === null) return null
      return [
        `inline gea::embedded::ui::NodeHandle mount_${sanitizeCppIdentifier(component.exportName)}(${fieldAccess.paramDecl}, const std::shared_ptr<NativeDisposer> &disposer) {`,
        `  return mount_${sanitizeCppIdentifier(resolved.component.exportName)}(${delegateStoreArg}, disposer);`,
        '}',
        '',
      ]
    }
    const resolvedRoot = parseTemplateRoot(resolved.component.template.html)
    const createResolvedRoot = resolvedRoot ? uiCreateNodeExpression(resolvedRoot) : null
    if (!resolvedRoot || !createResolvedRoot) return null
    const lines: string[] = [
      `inline gea::embedded::ui::NodeHandle mount_${sanitizeCppIdentifier(component.exportName)}(${fieldAccess.paramDecl}, const std::shared_ptr<NativeDisposer> &disposer) {`,
      `  auto root = ${createResolvedRoot};`,
    ]
    let nextNode = 0
    const nextVar = () => `__gea_node_${nextNode++}`
    emitInputButtonValueText(lines, 'root', resolvedRoot, nextVar)
    if (
      !emitTemplateElement({
        component: resolved.component,
        node: resolvedRoot,
        varName: 'root',
        components,
        storeFields,
        lines,
        nextVar,
        canMount,
        constants,
        propBindings: resolved.propBindings,
        storeMethods,
        fieldAccess,
        storeArrayFields,
      })
    ) {
      return null
    }
    lines.push('  return root;')
    lines.push('}')
    lines.push('')
    return lines
  }

  const createRoot = root ? uiCreateNodeExpression(root) : null
  if (!root || !createRoot) return null

  const lines: string[] = [
    `inline gea::embedded::ui::NodeHandle mount_${sanitizeCppIdentifier(component.exportName)}(${fieldAccess.paramDecl}, const std::shared_ptr<NativeDisposer> &disposer) {`,
    `  auto root = ${createRoot};`,
  ]

  let nextNode = 0
  const nextVar = () => `__gea_node_${nextNode++}`
  if (
    !emitTemplateElement({
      component,
      node: root,
      varName: 'root',
      components,
      storeFields,
      lines,
      nextVar,
      canMount,
      constants,
      propBindings: new Map(),
      storeMethods,
      fieldAccess,
      storeArrayFields,
    })
  ) {
    return null
  }

  lines.push('  return root;')
  lines.push('}')
  lines.push('')
  return lines
}

interface ResolvedWrapper {
  component: GeaIrComponent
  propBindings: Map<string, PropBinding>
}

// When a function component's JSX root is *another* component (e.g. `function
// SettingsRow() { return <View class="settings-row">...</View> }`), the IR
// emits `<span style="display:contents"><!--0--></span>` and a single mount
// slot for the wrapped component. Without resolving that, the compiler creates
// a real <span> at runtime, the wrapper props (class, style, onClick, …) and
// JSX children never bind on the wrapped element, and `<View class="x">y</View>`
// stops being equivalent to `<div class="x">y</div>`.
//
// This helper walks down such transparent layers, composing each mount slot's
// prop bindings via `mountPropBindings(slot, currentProps)` — that builds a
// `PropBinding` whose `parentProps` chain back to the original use site, so
// references like `<Text>{label}</Text>` inside `SettingsRow` still resolve
// `label` to whatever the caller passed (`<SettingsRow label="Bluetooth name"
// …/>`). The returned component is the first non-wrapper element, and the
// returned bindings are the merged props the emitter should apply to it.
function resolveTransparentWrappers(
  component: GeaIrComponent,
  components: GeaIrComponent[],
  propBindings: Map<string, PropBinding>,
  canMount: (component: GeaIrComponent) => boolean,
): ResolvedWrapper {
  let current = component
  let currentProps = propBindings
  // Bounded to keep degenerate IR (cycles) from spinning forever; real apps
  // never nest wrapper components anywhere close to this depth.
  for (let depth = 0; depth < 16; depth += 1) {
    const root = parseTemplateRoot(current.template.html)
    if (!root || !isTransparentDisplayContentsRoot(root)) break
    if (root.children.length !== 1 || current.template.slots.length !== 1) break
    const child = root.children[0]
    if (child.kind !== 'slot') break
    const slot = current.template.slots[0]
    if (slot.index !== child.index || slot.kind !== 'mount') break
    // <Image> uses a dedicated intrinsic path; leave it alone.
    if (mountSlotTag(slot) === 'Image') break
    const mountedChild = childForMount(slot, components)
    if (!mountedChild || mountedChild === current || !canMount(mountedChild)) break
    // Never peel INTO a ReactiveComponent: its template renders against its
    // own typed instance, not inline against the wrapper caller's bindings.
    if (mountedChild.reactiveState) break
    currentProps = mountPropBindings(slot, currentProps)
    current = mountedChild
  }
  return { component: current, propBindings: currentProps }
}

function transparentDisplayContentsImageMount(component: GeaIrComponent, root: TemplateElement): GeaIrSlot | null {
  if (!isTransparentDisplayContentsRoot(root)) return null
  if (root.children.length !== 1 || component.template.slots.length !== 1) return null
  const child = root.children[0]
  if (child.kind !== 'slot') return null
  const slot = component.template.slots[0]
  if (slot.index !== child.index || slot.kind !== 'mount' || mountSlotTag(slot) !== 'Image') return null
  return slot
}

function isTransparentDisplayContentsRoot(root: TemplateElement): boolean {
  const attrs = root.attrs ?? {}
  const keys = Object.keys(attrs)
  return keys.length === 1 && keys[0] === 'style' && /\bdisplay\s*:\s*contents\b/i.test(attrs.style)
}

export function canTemplateMountComponent(
  component: GeaIrComponent,
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  canMount: (component: GeaIrComponent) => boolean,
  storeMethods: StoreMethodPlan[] = [],
  storeArrayFields: StoreArrayFieldPlan[] = [],
): boolean {
  const root = parseTemplateRoot(component.template.html)
  if (!root || (!uiCreateNodeExpression(root) && !transparentDisplayContentsImageMount(component, root))) return false
  return canEmitTemplateElement(component, components, storeFields, constants, canMount, new Map(), storeMethods, storeArrayFields)
}

function emitTemplateElement(args: {
  component: GeaIrComponent
  node: TemplateElement
  varName: string
  components: GeaIrComponent[]
  storeFields: StoreFieldPlan[]
  lines: string[]
  nextVar: () => string
  canMount: (component: GeaIrComponent) => boolean
  constants: ConstantMap
  propBindings: Map<string, PropBinding>
  storeMethods: StoreMethodPlan[]
  fieldAccess?: FieldAccess
  storeArrayFields: StoreArrayFieldPlan[]
  disposerVar?: string
}): boolean {
  const { component, node, varName, storeFields, lines } = args
  // Mirror the filter in `canEmitTemplateElement`: drop root setup slots whose
  // expressions reference props the parent never passed. Without this, calling
  // `rootSetupLines` would fail on unresolvable `props.X` expressions even
  // though the can-emit pass had already filtered them out.
  const setupSlots = resolveRootSetupPropSlots(
    component.template.slots
      .filter((slot) => isRootSetupSlot(slot, args.constants))
      .filter((slot) => !slotReferencesUnboundProp(slot, args.propBindings)),
    args.propBindings,
    args.constants,
  )
  const setup = rootSetupLines(varName, setupSlots, storeFields, args.storeMethods, args.fieldAccess, node.tag, args.constants, activeDisposerVar(args))
  if (!setup) return false
  const slotsByIndex = new Map(component.template.slots.map((slot) => [slot.index, slot]))
  const pathVars = new Map<string, string>()
  const keyedListAnchors = new Map<string, { parentVar: string; anchorVar: string }>()
  pathVars.set('', varName)
  lines.push(...attributeLines(varName, { tag: node.tag, className: node.className, attrs: node.attrs }, component.template.slots))
  lines.push(...intrinsicStyleLines(varName, node))
  lines.push(...setup)
  emitInputButtonValueText(lines, varName, node, args.nextVar)
  if (!emitTemplateChildren({ ...args, slotsByIndex, pathVars, keyedListAnchors, path: [], disposerVar: activeDisposerVar(args) })) {
    return false
  }
  return emitReactiveSlots({
    component,
    components: args.components,
    storeFields,
    storeArrayFields: args.storeArrayFields,
    pathVars,
    lines,
    constants: args.constants,
    propBindings: args.propBindings,
    storeMethods: args.storeMethods,
    fieldAccess: args.fieldAccess,
    keyedListAnchors,
    disposerVar: activeDisposerVar(args),
  })
}

function canEmitTemplateElement(
  component: GeaIrComponent,
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  canMount: (component: GeaIrComponent) => boolean,
  propBindings: Map<string, PropBinding>,
  storeMethods: StoreMethodPlan[],
  storeArrayFields: StoreArrayFieldPlan[],
): boolean {
  // When this component is inlined by a parent that doesn't pass every prop
  // (typical for the gea-embedded `<View>`/`<Text>` wrappers: parents pass
  // `class` but not `style`, `onClick`, etc.), a slot whose expression is
  // just `props.X` for an unbound X is effectively undefined at runtime —
  // there's nothing to bind, nothing to emit. The previous implementation
  // failed the whole template here because `lowerTemplateExpression` can't
  // produce a value for `props.style` when `style` isn't in `propBindings`.
  // Filter those slots out instead, both for the root setup pass below and
  // for the per-slot loop. `emitTemplateElement` / `emitReactiveSlots` skip
  // the same set so emit-time stays in sync with the can-emit decision.
  const setupSlots = resolveRootSetupPropSlots(
    component.template.slots
      .filter((slot) => isRootSetupSlot(slot, constants))
      .filter((slot) => !slotReferencesUnboundProp(slot, propBindings)),
    propBindings,
    constants,
  )
  if (!rootSetupLines('root', setupSlots, storeFields, storeMethods, undefined, undefined, constants)) return false
  for (const slot of component.template.slots) {
    if (isRootSetupSlot(slot, constants)) continue
    if (slotReferencesUnboundProp(slot, propBindings)) continue
    if (slot.kind === 'mount') {
      if (mountSlotTag(slot) === 'Image') {
        if (!canEmitIntrinsicImageMount(slot, storeFields, constants, propBindings)) return false
        continue
      }
      const child = childForMount(slot, components)
      const childProps = mountPropBindings(slot, propBindings)
      if (!child || (child === component && childProps.size === 0)) return false
      if (childProps.size > 0) {
        // Props mean inlining, which a reactive child can't do (its fields
        // live on its own typed instance) — mirror of the emission guard.
        if (child.reactiveState) return false
        const childRoot = parseTemplateRoot(child.template.html)
        if (!childRoot || !uiCreateNodeExpression(childRoot)) return false
        if (!canEmitTemplateElement(child, components, storeFields, constants, canMount, childProps, storeMethods, storeArrayFields)) return false
      } else if (!canMount(child) && (child.reactiveState || !canGenericChildMount(child, childProps))) {
        return false
      }
      continue
    }
    if (slot.kind === 'keyed-list') {
      if (
        !canEmitStaticKeyedList({ component, slot, components, storeFields, storeArrayFields, constants, canMount, propBindings, storeMethods }) &&
        !canEmitDynamicKeyedListSlot({
          component,
          slot,
          components,
          storeFields,
          storeArrayFields,
          constants,
          propBindings,
          storeMethods,
        })
      ) return false
      continue
    }
    if (slot.kind === 'text') {
      const childrenBinding = childrenBindingForPropSlot(slot, propBindings)
      if (childrenBinding?.childrenTemplate) {
        if (!canEmitTemplateFragment(component, childrenBinding.childrenTemplate, components, storeFields, constants, canMount, childrenBinding.parentProps, storeMethods, storeArrayFields)) return false
        continue
      }
      const lowered = lowerTemplateExpression(slot.expr, storeFields, constants, propBindings)
      if (!lowered || lowered.fields.some((field) => !field.readerName)) return false
      continue
    }
    if (slot.kind === 'class') {
      if (!canLowerTemplateClassSlot(slot, storeFields, constants, propBindings)) return false
      continue
    }
    if (slot.kind === 'style') {
      if (!canLowerTemplateStyleSlot(slot, storeFields, constants, propBindings)) return false
      continue
    }
    if (slot.kind === 'conditional') {
      if (!canEmitConditionalSlot(component, slot, components, storeFields, constants, canMount, propBindings, storeMethods, storeArrayFields)) return false
      continue
    }
    if (slot.kind === 'attr') {
      if (!canEmitTemplateAttrSlot(slot, storeFields, constants, propBindings)) return false
      continue
    }
    if (slot.kind === 'event') {
      if (!eventSetupLines('root', slot, storeFields, storeMethods, undefined, undefined, constants)) return false
      continue
    }
    // `kind === 'value'` / `kind === 'bool'` are the IR's reactive-attribute
    // bindings used when a JSX attribute is wired to a store expression:
    // `value={todo.draft}` on `<input type="range">` (value) or
    // `checked={store.on}` on `<input type="checkbox">` (bool). Both carry the
    // same `expr` / `payload.attrName` shape a `text` slot does — lower the
    // expression through the standard store-field reader pipeline and emit a
    // reactiveApply that writes back via `setAttribute` (see
    // `emitReactiveSlots`). The macOS renderer's applyInputProps /
    // applySwitchProps read that attribute back on every tree sync.
    if (slot.kind === 'value' || slot.kind === 'bool') {
      if (!slotAttrName(slot)) return false
      const lowered = lowerTemplateExpression(slot.expr, storeFields, constants, propBindings)
      if (!lowered || lowered.fields.some((field) => !field.readerName)) return false
      continue
    }
    return false
  }
  return true
}

function emitTemplateChildren(args: {
  component: GeaIrComponent
  node: TemplateElement
  varName: string
  path: number[]
  components: GeaIrComponent[]
  slotsByIndex: Map<number, GeaIrSlot>
  pathVars: Map<string, string>
  lines: string[]
  nextVar: () => string
  canMount: (component: GeaIrComponent) => boolean
  storeFields: StoreFieldPlan[]
  storeArrayFields: StoreArrayFieldPlan[]
  storeMethods: StoreMethodPlan[]
  constants: ConstantMap
  propBindings: Map<string, PropBinding>
  textTargetVar?: string
  fieldAccess?: FieldAccess
  keyedListAnchors?: Map<string, { parentVar: string; anchorVar: string }>
  disposerVar?: string
}): boolean {
  const { node, varName, path, components, slotsByIndex, pathVars, lines, nextVar, canMount, propBindings } = args
  const disposerVar = activeDisposerVar(args)
  for (let index = 0; index < node.children.length; index += 1) {
    const child = node.children[index]
    const childPath = [...path, index]
    if (child.kind === 'element') {
      const childVar = nextVar()
      const createChild = uiCreateNodeExpression(child)
      if (!createChild) return false
      lines.push(`  auto ${childVar} = ${createChild};`)
      pathVars.set(pathKey(childPath), childVar)
      lines.push(...attributeLines(childVar, { tag: child.tag, className: child.className, attrs: child.attrs }))
      lines.push(...intrinsicStyleLines(childVar, child))
      lines.push(`  ${varName}.appendChild(${childVar});`)
      emitInputButtonValueText(lines, childVar, child, nextVar)
      if (!emitTemplateChildren({ ...args, node: child, varName: childVar, path: childPath })) return false
      continue
    }
    if (child.kind === 'text') {
      const text = renderableTemplateText(child.text)
      if (!text) continue
      // The IR uses a literal `0` character as the text anchor for `directText`
      // slots (see walk.ts: `html += '0' // placeholder, overwritten by ...`).
      // It is meant to be replaced by the reactive text emit. If we treat it
      // as literal content the user sees a bare "0" on screen wherever a
      // reactive text slot lives — and *especially* wherever such a slot
      // ends up unbound (e.g. `<View>{props.children}</View>` inlined into a
      // parent that didn't pass children).
      const placeholderSlot = text === '0'
        ? Array.from(slotsByIndex.values()).find(
            (s) => s.directText === true && s.kind === 'text' && pathKey(s.walk) === pathKey(childPath),
          ) ?? null
        : null
      const childrenBinding = placeholderSlot ? childrenBindingForPropSlot(placeholderSlot, propBindings) : null
      const childrenTemplate = childrenBinding?.childrenTemplate ?? null
      if (childrenTemplate && childrenBinding) {
        const textTargetVar = canUseTextNodeForElement(node) ? varName : undefined
      if (
          !emitTemplateFragment({
            ...args,
            template: childrenTemplate,
            parentVar: varName,
            textTargetVar,
            // Emit the children in the scope where the JSX was authored — the
            // wrapper's caller — not in the inlined element's scope.
            propBindings: childrenBinding.parentProps,
          })
        ) {
          return false
        }
        continue
      }
      if (args.textTargetVar && node.tag === '#fragment') {
        if (!placeholderSlot) lines.push(`  ${args.textTargetVar}.setText(${cppString(text)});`)
        pathVars.set(pathKey(childPath), args.textTargetVar)
        continue
      }
      // A directText placeholder ("0") carries a reactive expr. Seed its current
      // value here (before append / alongside the parent) so a parent whose only
      // content is this value measures a non-zero size at first layout; otherwise
      // it stays 0-sized and invisible. emitReactiveSlots still binds it for updates.
      const seedPlaceholder = (target: string): void => {
        if (!placeholderSlot) return
        const ph = lowerTemplateExpression(placeholderSlot.expr, args.storeFields, args.constants, propBindings)
        if (ph && !ph.fields.some((field) => !field.readerName)) {
          emitSyncSeed(lines, ph, [`${target}.setText((${loweredAsString(ph)}).c_str());`])
        }
      }
      if (canUseTextNodeForElement(node)) {
        if (!placeholderSlot) lines.push(`  ${varName}.setText(${cppString(text)});`)
        else seedPlaceholder(varName)
        pathVars.set(pathKey(childPath), varName)
      } else {
        const textVar = nextVar()
        lines.push(`  auto ${textVar} = gea::embedded::ui::Document::instance().createText();`)
        if (!placeholderSlot) lines.push(`  ${textVar}.setText(${cppString(text)});`)
        else seedPlaceholder(textVar)
        lines.push(`  ${varName}.appendChild(${textVar});`)
        pathVars.set(pathKey(childPath), textVar)
      }
      continue
    }
    const slot = slotsByIndex.get(child.index)
    if (!slot) return false
    // If the slot references a prop the parent didn't pass, treat its slot
    // position as empty. Without this we'd emit an unused text/anchor child
    // that the matching `emitReactiveSlots` then skips, producing a dead DOM
    // node. Common for the gea-embedded `<View>` template's `props.children`
    // when the parent supplies no children.
    if (slotReferencesUnboundProp(slot, propBindings)) continue
    if (slot.kind === 'text') {
      if (args.textTargetVar && node.tag === '#fragment') {
        pathVars.set(pathKey(childPath), args.textTargetVar)
        continue
      }
      const textVar = nextVar()
      lines.push(`  auto ${textVar} = gea::embedded::ui::Document::instance().createText();`)
      // Seed the current value BEFORE appendChild so a parent whose only child is
      // this reactive text measures a non-zero size at first layout (matching how
      // static text is emitted). emitReactiveSlots still binds it for updates.
      const seedLowered = lowerTemplateExpression(slot.expr, args.storeFields, args.constants, propBindings)
      if (seedLowered && !seedLowered.fields.some((field) => !field.readerName)) {
        emitSyncSeed(lines, seedLowered, [`${textVar}.setText((${loweredAsString(seedLowered)}).c_str());`])
      }
      lines.push(`  ${varName}.appendChild(${textVar});`)
      pathVars.set(pathKey(childPath), textVar)
      continue
    }
    if (slot.kind === 'keyed-list') {
      if (emitStaticKeyedList({
        component: args.component,
        components: args.components,
        storeFields: args.storeFields,
        storeMethods: args.storeMethods,
        storeArrayFields: args.storeArrayFields,
        constants: args.constants,
        propBindings,
        fieldAccess: args.fieldAccess,
        lines,
        parentVar: varName,
        slot,
        nextVar,
        canMount: args.canMount,
        disposerVar,
      })) {
        continue
      }
      if (
        !canEmitDynamicKeyedListSlot({
          component: args.component,
          slot,
          components: args.components,
          storeFields: args.storeFields,
          storeArrayFields: args.storeArrayFields,
          constants: args.constants,
          propBindings,
          storeMethods: args.storeMethods,
        })
      ) return false
      const anchorVar = nextVar()
      lines.push(`  auto ${anchorVar} = gea::embedded::ui::Document::instance().createView();`)
      lines.push(`  ${anchorVar}.setTagName("#comment");`)
      lines.push(`  ${anchorVar}.style().display(gea::embedded::ui::kDisplayNone);`)
      lines.push(`  ${varName}.appendChild(${anchorVar});`)
      args.keyedListAnchors?.set(pathKey(childPath), { parentVar: varName, anchorVar })
      continue
    }
    if (slot.kind === 'conditional') {
      if (!emitConditionalSlot({ ...args, slot, parentVar: varName })) return false
      continue
    }
    if (slot.kind !== 'mount') return false
    if (mountSlotTag(slot) === 'Image') {
      const imageVar = nextVar()
      lines.push(`  auto ${imageVar} = gea::embedded::ui::Document::instance().createImage();`)
      lines.push(...attributeLines(imageVar, { tag: 'img' }))
      if (!emitIntrinsicImageMount(lines, imageVar, slot, args.storeFields, args.constants, propBindings, disposerVar)) return false
      lines.push(`  ${varName}.appendChild(${imageVar});`)
      pathVars.set(pathKey(childPath), imageVar)
      continue
    }
    const mountedChild = childForMount(slot, components)
    if (!mountedChild) return false
    const childProps = mountPropBindings(slot, propBindings)
    if (childProps.size === 0) {
      const fieldAccess = args.fieldAccess ?? BOXED_FIELD_ACCESS
      if (canMount(mountedChild)) {
        const childStoreArg = fieldAccess.childMountStoreArg(mountedChild)
        if (childStoreArg === null) return false
        lines.push(`  ${varName}.appendChild(mount_${sanitizeCppIdentifier(mountedChild.exportName)}(${childStoreArg}, ${disposerVar}));`)
        continue
      }
      // The generic fallback instantiates the child's compiled C++ class. In
      // typed self-store mode the child may exist only in the IR (its module
      // tree-shaken from the bundle once the lean parent's template moved to
      // the IR), so there is no class to construct — fail the renderer so the
      // component surfaces a diagnostic instead of emitting uncompilable code.
      // A reactive child must never go through it either: its compiled class
      // has a neutralized __gea_to_value(), so fn_mount renders NOTHING.
      if (mountedChild.reactiveState || fieldAccess.typed || !canGenericChildMount(mountedChild, childProps)) return false
      emitGenericChildMount(lines, varName, mountedChild, nextVar, disposerVar)
      continue
    }
    // Inlining renders the child's template against the PARENT's store access —
    // impossible for a ReactiveComponent child, whose fields live on its own
    // typed instance.
    if (mountedChild.reactiveState) return false
    // Resolve any transparent wrapper chain so the inlined child renders on
    // the wrapped element directly. Without this, mounting a wrapper component
    // like `<SettingsRow label=… value=…/>` would create an extra
    // display:contents span around the actual <div>, and the wrapper's prop
    // bindings would never reach the inner element.
    const resolvedChild = resolveTransparentWrappers(mountedChild, components, childProps, canMount)
    const effectiveChild = resolvedChild.component
    const effectiveProps = resolvedChild.propBindings
    const mountedRoot = parseTemplateRoot(effectiveChild.template.html)
    const createMountedRoot = mountedRoot ? uiCreateNodeExpression(mountedRoot) : null
    if (!mountedRoot || !createMountedRoot) return false
    const childVar = nextVar()
    lines.push(`  auto ${childVar} = ${createMountedRoot};`)
    lines.push(`  ${varName}.appendChild(${childVar});`)
    emitInputButtonValueText(lines, childVar, mountedRoot, nextVar)
    if (
      !emitTemplateElement({
        component: effectiveChild,
        node: mountedRoot,
        varName: childVar,
        components,
        storeFields: args.storeFields,
        lines,
        nextVar,
        canMount,
        constants: args.constants,
        propBindings: effectiveProps,
        storeMethods: args.storeMethods,
        fieldAccess: args.fieldAccess,
        storeArrayFields: args.storeArrayFields,
        disposerVar,
      })
    ) {
      return false
    }
  }
  return true
}

function canGenericChildMount(component: GeaIrComponent, propBindings: Map<string, PropBinding>): boolean {
  return propBindings.size === 0 && parseTemplateRoot(component.template.html) !== null
}

function emitGenericChildMount(
  lines: string[],
  parentVar: string,
  component: GeaIrComponent,
  nextVar: () => string,
  disposerVar: string,
): void {
  const componentVar = `${nextVar()}_component`
  const addVar = `${componentVar}_dispose`
  lines.push(`  auto ${componentVar} = std::make_shared<${sanitizeCppIdentifier(component.exportName)}>();`)
  lines.push(`  ${componentVar}->render(gea_ir::hostDocumentNodeValue(${parentVar}));`)
  lines.push(`  auto ${addVar} = ${disposerVar};`)
  lines.push(`  if (${addVar}) {`)
  lines.push(`    ${addVar}->add([${componentVar}]() mutable -> void { if (${componentVar}) ${componentVar}->dispose(); });`)
  lines.push('  }')
}

function emitGenericChildAsNode(
  lines: string[],
  varName: string,
  component: GeaIrComponent,
  nextVar: () => string,
  disposerVar: string,
): void {
  const componentVar = `${nextVar()}_component`
  const addVar = `${componentVar}_dispose`
  lines.push(`  auto ${varName} = gea::embedded::ui::Document::instance().createView();`)
  lines.push(`  ${varName}.setTagName("span");`)
  lines.push(`  auto ${componentVar} = std::make_shared<${sanitizeCppIdentifier(component.exportName)}>();`)
  lines.push(`  ${componentVar}->render(gea_ir::hostDocumentNodeValue(${varName}));`)
  lines.push(`  auto ${addVar} = ${disposerVar};`)
  lines.push(`  if (${addVar}) {`)
  lines.push(`    ${addVar}->add([${componentVar}]() mutable -> void { if (${componentVar}) ${componentVar}->dispose(); });`)
  lines.push('  }')
}

function emitReactiveSlots(args: {
  component: GeaIrComponent
  components: GeaIrComponent[]
  slots?: GeaIrSlot[]
  storeFields: StoreFieldPlan[]
  storeArrayFields: StoreArrayFieldPlan[]
  storeMethods: StoreMethodPlan[]
  pathVars: Map<string, string>
  lines: string[]
  constants: ConstantMap
  propBindings: Map<string, PropBinding>
  fieldAccess?: FieldAccess
  keyedListAnchors?: Map<string, { parentVar: string; anchorVar: string }>
  disposerVar?: string
}): boolean {
  const { component, storeFields, pathVars, lines, constants, propBindings } = args
  const fieldAccess = args.fieldAccess ?? BOXED_FIELD_ACCESS
  const disposerVar = activeDisposerVar(args)
  for (const slot of args.slots ?? component.template.slots) {
    if (slot.kind === 'keyed-list' && isStaticKeyedListSource(slot, constants)) continue
    // Anchorless keyed-list slots (the map is the element's sole content) are
    // attached to the element itself. The typed self-store path renders them
    // inline (rebuild-on-rev keyed rows); the boxed path keeps its existing
    // behavior (dedicated keyed renderers handle list-rooted components).
    if (slot.kind === 'keyed-list' && fieldAccess.typed) {
      const target = pathVars.get(pathKey(slot.walk))
      if (!target) return false
      if (!emitTypedKeyedListSlot(lines, target, slot, component, constants, disposerVar)) return false
      continue
    }
    if (slot.kind === 'keyed-list') {
      const anchor = args.keyedListAnchors?.get(pathKey(slot.walk))
      const target = anchor?.parentVar ?? pathVars.get(pathKey(slot.walk))
      if (!target) return false
      if (
        !emitDynamicKeyedListSlot({
          component,
          slot,
          target,
          insertBefore: anchor?.anchorVar,
          components: args.components,
          storeFields,
          storeArrayFields: args.storeArrayFields,
          constants,
          propBindings,
          storeMethods: args.storeMethods,
          lines,
          disposerVar,
        })
      ) return false
      continue
    }
    if (isRootSetupSlot(slot, constants) || slot.kind === 'mount' || slot.kind === 'keyed-list' || slot.kind === 'conditional') continue
    if (slotReferencesUnboundProp(slot, propBindings)) continue
    if (childrenTemplateForPropSlot(slot, propBindings)) continue
    const target = pathVars.get(pathKey(slot.walk))
    if (!target) return false
    if (slot.kind === 'text') {
      const lowered = lowerTemplateExpression(slot.expr, storeFields, constants, propBindings)
      if (!lowered || lowered.fields.some((field) => !field.readerName)) return false
      emitReactiveApply(lines, target, lowered, `${target}.setText((${loweredAsString(lowered)}).c_str());`, fieldAccess, [], disposerVar)
      continue
    }
    if (slot.kind === 'class') {
      if (!emitTemplateClassSlot(lines, target, slot, storeFields, constants, propBindings, fieldAccess, disposerVar)) return false
      continue
    }
    if (slot.kind === 'style') {
      if (!emitTemplateStyleSlot(lines, target, slot, storeFields, constants, propBindings, fieldAccess, disposerVar)) return false
      continue
    }
    if (slot.kind === 'attr') {
      const attrLines = attributeSlotLines(target, slot)
      if (attrLines) {
        lines.push(...attrLines)
      } else if (!emitTemplateAttrSlot(lines, target, slot, storeFields, constants, propBindings, fieldAccess, disposerVar)) {
        return false
      }
      continue
    }
    if (slot.kind === 'event') {
      const eventLines = eventSetupLines(target, slot, storeFields, args.storeMethods, fieldAccess, undefined, constants, disposerVar)
      if (!eventLines) return false
      lines.push(...eventLines)
      continue
    }
    // Reactive `value=` (and other reactive-attr-shaped) slots: track the
    // store field via emitReactiveApply and push the latest stringified
    // value into the DOM attribute. The macOS renderer's applyInputProps
    // reads that attribute back into NSTextField's stringValue on every
    // tree sync (except when the field is first responder, to avoid
    // clobbering an in-flight edit).
    if (slot.kind === 'value' || slot.kind === 'bool') {
      const attrName = slotAttrName(slot)
      if (!attrName) return false
      const lowered = lowerTemplateExpression(slot.expr, storeFields, constants, propBindings)
      if (!lowered || lowered.fields.some((field) => !field.readerName)) return false
      emitReactiveApply(
        lines,
        target,
        lowered,
        `${target}.setAttribute(${cppString(attrName)}, (${loweredAsString(lowered)}).c_str());`,
        fieldAccess,
        [],
        disposerVar,
      )
      continue
    }
    return false
  }
  return true
}

function canEmitTemplateFragment(
  component: GeaIrComponent,
  template: GeaIrTemplate,
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  canMount: (component: GeaIrComponent) => boolean,
  propBindings: Map<string, PropBinding>,
  storeMethods: StoreMethodPlan[],
  storeArrayFields: StoreArrayFieldPlan[],
): boolean {
  const children = parseTemplateFragmentChildren(template.html)
  if (!children || !canEmitTemplateChildStructure(children)) return false
  for (const slot of template.slots) {
    if (slotReferencesUnboundProp(slot, propBindings)) continue
    if (slot.kind === 'mount') {
      if (mountSlotTag(slot) === 'Image') {
        if (!canEmitIntrinsicImageMount(slot, storeFields, constants, propBindings)) return false
        continue
      }
      const child = childForMount(slot, components)
      const childProps = mountPropBindings(slot, propBindings)
      if (!child || (child === component && childProps.size === 0)) return false
      if (childProps.size > 0) {
        // Mirror of canEmitTemplateElement: props mean inlining, which a
        // reactive child can't do.
        if (child.reactiveState) return false
        const childRoot = parseTemplateRoot(child.template.html)
        if (!childRoot || !uiCreateNodeExpression(childRoot)) return false
        if (!canEmitTemplateElement(child, components, storeFields, constants, canMount, childProps, storeMethods, storeArrayFields)) return false
      } else if (!canMount(child)) {
        return false
      }
      continue
    }
    if (slot.kind === 'keyed-list') {
      if (
        !canEmitStaticKeyedList({ component, slot, components, storeFields, storeArrayFields, constants, canMount, propBindings, storeMethods }) &&
        !canEmitDynamicKeyedListSlot({
          component,
          slot,
          components,
          storeFields,
          storeArrayFields,
          constants,
          propBindings,
          storeMethods,
        })
      ) return false
      continue
    }
    if (slot.kind === 'conditional') {
      if (!canEmitConditionalSlot(component, slot, components, storeFields, constants, canMount, propBindings, storeMethods, storeArrayFields)) return false
      continue
    }
    if (slot.kind === 'text') {
      const childrenBinding = childrenBindingForPropSlot(slot, propBindings)
      if (childrenBinding?.childrenTemplate) {
        if (!canEmitTemplateFragment(component, childrenBinding.childrenTemplate, components, storeFields, constants, canMount, childrenBinding.parentProps, storeMethods, storeArrayFields)) return false
        continue
      }
      const lowered = lowerTemplateExpression(slot.expr, storeFields, constants, propBindings)
      if (!lowered || lowered.fields.some((field) => !field.readerName)) return false
      continue
    }
    if (slot.kind === 'class') {
      if (!canLowerTemplateClassSlot(slot, storeFields, constants, propBindings)) return false
      continue
    }
    if (slot.kind === 'style') {
      if (!canLowerTemplateStyleSlot(slot, storeFields, constants, propBindings)) return false
      continue
    }
    if (slot.kind === 'attr') {
      if (!canEmitTemplateAttrSlot(slot, storeFields, constants, propBindings)) return false
      continue
    }
    if (slot.kind === 'event') {
      if (!eventSetupLines('root', slot, storeFields, storeMethods, undefined, undefined, constants)) return false
      continue
    }
    if (slot.kind === 'value' || slot.kind === 'bool') {
      if (!slotAttrName(slot)) return false
      const lowered = lowerTemplateExpression(slot.expr, storeFields, constants, propBindings)
      if (!lowered || lowered.fields.some((field) => !field.readerName)) return false
      continue
    }
    return false
  }
  return true
}

function canEmitTemplateChildStructure(children: TemplateChild[]): boolean {
  for (const child of children) {
    if (child.kind !== 'element') continue
    if (!uiCreateNodeExpression(child)) return false
    if (!canEmitTemplateChildStructure(child.children)) return false
  }
  return true
}

function canEmitConditionalSlot(
  component: GeaIrComponent,
  slot: GeaIrSlot,
  components: GeaIrComponent[],
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  canMount: (component: GeaIrComponent) => boolean,
  propBindings: Map<string, PropBinding>,
  storeMethods: StoreMethodPlan[],
  storeArrayFields: StoreArrayFieldPlan[],
): boolean {
  const condition = lowerTemplateExpression(slot.expr, storeFields, constants, propBindings)
  if (!condition || condition.fields.some((field) => !field.readerName)) return false
  const templates = conditionalBranchTemplates(slot)
  if (!templates || !templates.consequent) return false
  if (!canEmitTemplateFragment(component, templates.consequent, components, storeFields, constants, canMount, propBindings, storeMethods, storeArrayFields)) return false
  return !templates.alternate || canEmitTemplateFragment(component, templates.alternate, components, storeFields, constants, canMount, propBindings, storeMethods, storeArrayFields)
}

function emitTemplateFragment(args: {
  component: GeaIrComponent
  template: GeaIrTemplate
  parentVar: string
  components: GeaIrComponent[]
  storeFields: StoreFieldPlan[]
  storeArrayFields: StoreArrayFieldPlan[]
  storeMethods: StoreMethodPlan[]
  lines: string[]
  nextVar: () => string
  canMount: (component: GeaIrComponent) => boolean
  constants: ConstantMap
  propBindings: Map<string, PropBinding>
  textTargetVar?: string
  fieldAccess?: FieldAccess
  disposerVar?: string
}): boolean {
  const children = parseTemplateFragmentChildren(args.template.html)
  if (!children) return false
  const slotsByIndex = new Map(args.template.slots.map((slot) => [slot.index, slot]))
  const pathVars = new Map<string, string>()
  const keyedListAnchors = new Map<string, { parentVar: string; anchorVar: string }>()
  const fragment: TemplateElement = { kind: 'element', tag: '#fragment', children }
  if (
    !emitTemplateChildren({
      component: args.component,
      node: fragment,
      varName: args.parentVar,
      components: args.components,
      storeFields: args.storeFields,
      storeMethods: args.storeMethods,
      lines: args.lines,
      nextVar: args.nextVar,
      canMount: args.canMount,
      constants: args.constants,
      propBindings: args.propBindings,
      slotsByIndex,
      pathVars,
      keyedListAnchors,
      path: [],
      textTargetVar: args.textTargetVar,
      fieldAccess: args.fieldAccess,
      storeArrayFields: args.storeArrayFields,
      disposerVar: activeDisposerVar(args),
    })
  ) {
    return false
  }
  return emitReactiveSlots({
    component: args.component,
    components: args.components,
    slots: args.template.slots,
    storeFields: args.storeFields,
    storeArrayFields: args.storeArrayFields,
    storeMethods: args.storeMethods,
    pathVars,
    lines: args.lines,
    constants: args.constants,
    propBindings: args.propBindings,
    fieldAccess: args.fieldAccess,
    keyedListAnchors,
    disposerVar: activeDisposerVar(args),
  })
}

function emitConditionalSlot(args: {
  component: GeaIrComponent
  slot: GeaIrSlot
  parentVar: string
  components: GeaIrComponent[]
  storeFields: StoreFieldPlan[]
  storeArrayFields: StoreArrayFieldPlan[]
  storeMethods: StoreMethodPlan[]
  lines: string[]
  nextVar: () => string
  canMount: (component: GeaIrComponent) => boolean
  constants: ConstantMap
  propBindings: Map<string, PropBinding>
  fieldAccess?: FieldAccess
  keyedListAnchors?: Map<string, { parentVar: string; anchorVar: string }>
  disposerVar?: string
}): boolean {
  const condition = lowerTemplateExpression(args.slot.expr, args.storeFields, args.constants, args.propBindings)
  const templates = conditionalBranchTemplates(args.slot)
  if (!condition || condition.fields.some((field) => !field.readerName) || !templates?.consequent) return false
  const disposerVar = activeDisposerVar(args)
  const suffix = args.nextVar().replace(/^__gea_node_/, '')
  const branchRef = `__gea_branch_${suffix}`
  const stateRef = `__gea_branch_state_${suffix}`
  const branchDisposerRef = `__gea_branch_disposer_${suffix}`
  const anchorRef = `__gea_branch_anchor_${suffix}`
  const makeTrue = `__gea_make_true_${suffix}`
  const makeFalse = `__gea_make_false_${suffix}`
  const hasFalseBranch = templates.alternate ? 'true' : 'false'

  args.lines.push(`  auto ${branchRef} = std::make_shared<gea::embedded::ui::NodeHandle>();`)
  args.lines.push(`  auto ${stateRef} = std::make_shared<int>(-1);`)
  args.lines.push(`  auto ${branchDisposerRef} = std::make_shared<std::shared_ptr<NativeDisposer>>();`)
  args.lines.push(`  auto ${anchorRef} = gea::embedded::ui::Document::instance().createView();`)
  args.lines.push(`  ${anchorRef}.setTagName("#comment");`)
  args.lines.push(`  ${anchorRef}.style().display(gea::embedded::ui::kDisplayNone);`)
  args.lines.push(`  ${args.parentVar}.appendChild(${anchorRef});`)
  if (!emitTemplateFactory(args, makeTrue, templates.consequent, branchDisposerRef)) return false
  if (!emitTemplateFactory(args, makeFalse, templates.alternate ?? null, branchDisposerRef)) return false
  emitReactiveApply(
    args.lines,
    args.parentVar,
    condition,
    [
      `const bool __gea_next_cond = ${loweredAsBoolean(condition)};`,
      'const int __gea_next_state = __gea_next_cond ? 1 : 0;',
      `const bool __gea_next_has_branch = __gea_next_cond || ${hasFalseBranch};`,
      `const bool __gea_branch_attached = ${branchRef}->id() >= 0 && gea::embedded::ui::Tree::instance().containsNode(${args.parentVar}.id(), ${branchRef}->id());`,
      `const bool __gea_branch_has_disposer = static_cast<bool>(*${branchDisposerRef});`,
      `if (*${stateRef} == __gea_next_state && (!__gea_next_has_branch || (__gea_branch_attached && __gea_branch_has_disposer))) return;`,
      `if (*${branchDisposerRef}) { (*${branchDisposerRef})->dispose(); ${branchDisposerRef}->reset(); }`,
      `if (${branchRef}->id() >= 0) { const bool __gea_branch_attached_now = gea::embedded::ui::Tree::instance().containsNode(${args.parentVar}.id(), ${branchRef}->id()); if (__gea_branch_attached_now) ${branchRef}->remove(); *${branchRef} = gea::embedded::ui::NodeHandle(); }`,
      `*${stateRef} = __gea_next_state;`,
      `*${branchRef} = __gea_next_cond ? ${makeTrue}() : ${makeFalse}();`,
      `if (${branchRef}->id() >= 0) ${args.parentVar}.insertBefore(*${branchRef}, ${anchorRef});`,
    ],
    args.fieldAccess,
    [branchRef, stateRef, branchDisposerRef, anchorRef, makeTrue, makeFalse],
    disposerVar,
  )
  return true
}

function emitTemplateFactory(
  args: {
    component: GeaIrComponent
    components: GeaIrComponent[]
    storeFields: StoreFieldPlan[]
    storeArrayFields: StoreArrayFieldPlan[]
    storeMethods: StoreMethodPlan[]
    lines: string[]
    nextVar: () => string
    canMount: (component: GeaIrComponent) => boolean
    constants: ConstantMap
    propBindings: Map<string, PropBinding>
    fieldAccess?: FieldAccess
    disposerVar?: string
  },
  name: string,
  template: GeaIrTemplate | null,
  scopedDisposerRef?: string,
): boolean {
  const parentDisposerVar = activeDisposerVar(args)
  const branchDisposerVar = scopedDisposerRef ? `__gea_${name}_disposer` : parentDisposerVar
  const captures = ['store', parentDisposerVar]
  if (scopedDisposerRef) captures.push(scopedDisposerRef)
  args.lines.push(`  auto ${name} = [${[...new Set(captures)].join(', ')}]() mutable -> gea::embedded::ui::NodeHandle {`)
  if (!template) {
    args.lines.push('    return gea::embedded::ui::NodeHandle();')
    args.lines.push('  };')
    return true
  }
  if (scopedDisposerRef) {
    args.lines.push(`    auto __gea_parent_disposer = ${parentDisposerVar};`)
    args.lines.push(`    auto __gea_branch_disposer = __gea_parent_disposer ? __gea_parent_disposer->child() : std::make_shared<NativeDisposer>();`)
    args.lines.push(`    *${scopedDisposerRef} = __gea_branch_disposer;`)
    args.lines.push(`    auto ${branchDisposerVar} = __gea_branch_disposer;`)
  }
  const rootVar = `__gea_${name}_root`
  if (!emitTemplateAsNode({ ...args, template, rootVar, disposerVar: branchDisposerVar })) return false
  args.lines.push(`    return ${rootVar};`)
  args.lines.push('  };')
  return true
}

function emitTemplateAsNode(args: {
  component: GeaIrComponent
  template: GeaIrTemplate
  rootVar: string
  components: GeaIrComponent[]
  storeFields: StoreFieldPlan[]
  storeArrayFields: StoreArrayFieldPlan[]
  storeMethods: StoreMethodPlan[]
  lines: string[]
  nextVar: () => string
  canMount: (component: GeaIrComponent) => boolean
  constants: ConstantMap
  propBindings: Map<string, PropBinding>
  fieldAccess?: FieldAccess
  disposerVar?: string
}): boolean {
  const children = parseTemplateFragmentChildren(args.template.html)
  if (!children) return false
  if (children.length === 1) {
    const child = children[0]
    if (child.kind === 'element') {
      const createRoot = uiCreateNodeExpression(child)
      if (!createRoot) return false
      args.lines.push(`  auto ${args.rootVar} = ${createRoot};`)
      // A conditional / mount branch template carries its OWN slots. Resolve slot
      // indices against THIS template, not the parent component's — emitTemplateElement
      // otherwise rebuilds slotsByIndex from `component.template.slots`, so a
      // single-root-element branch reuses the PARENT's slots and an internal slot
      // index can resolve back to the conditional slot itself: infinite recursion in
      // emitTemplateChildren. (The multi-child path, emitTemplateFragment, already
      // scopes slots to args.template.slots; this mirrors it for the 1-root case.)
      return emitTemplateElement({
        component: { ...args.component, template: args.template },
        node: child,
        varName: args.rootVar,
        components: args.components,
        storeFields: args.storeFields,
        storeMethods: args.storeMethods,
        lines: args.lines,
        nextVar: args.nextVar,
        canMount: args.canMount,
        constants: args.constants,
        propBindings: args.propBindings,
        fieldAccess: args.fieldAccess,
        storeArrayFields: args.storeArrayFields,
        disposerVar: activeDisposerVar(args),
      })
    }
    if (child.kind === 'slot') {
      const slot = args.template.slots.find((candidate) => candidate.index === child.index)
      if (slot?.kind === 'mount') return emitMountSlotAsNode(args, slot, args.rootVar)
    }
  }

  args.lines.push(`  auto ${args.rootVar} = gea::embedded::ui::Document::instance().createView();`)
  args.lines.push(`  ${args.rootVar}.setTagName("span");`)
  return emitTemplateFragment({ ...args, parentVar: args.rootVar })
}

function emitMountSlotAsNode(
  args: {
    component: GeaIrComponent
    components: GeaIrComponent[]
    storeFields: StoreFieldPlan[]
    storeArrayFields: StoreArrayFieldPlan[]
    storeMethods: StoreMethodPlan[]
    lines: string[]
    nextVar: () => string
    canMount: (component: GeaIrComponent) => boolean
    constants: ConstantMap
    propBindings: Map<string, PropBinding>
    fieldAccess?: FieldAccess
    disposerVar?: string
  },
  slot: GeaIrSlot,
  varName: string,
): boolean {
  const disposerVar = activeDisposerVar(args)
  if (mountSlotTag(slot) === 'Image') {
    args.lines.push(`  auto ${varName} = gea::embedded::ui::Document::instance().createImage();`)
    args.lines.push(...attributeLines(varName, { tag: 'img' }))
    return emitIntrinsicImageMount(args.lines, varName, slot, args.storeFields, args.constants, args.propBindings, disposerVar)
  }
  const mountedChild = childForMount(slot, args.components)
  if (!mountedChild) return false
  const childProps = mountPropBindings(slot, args.propBindings)
  if (childProps.size === 0) {
    if (!args.canMount(mountedChild)) {
      if (args.fieldAccess?.typed || mountedChild.reactiveState || !canGenericChildMount(mountedChild, childProps)) return false
      emitGenericChildAsNode(args.lines, varName, mountedChild, args.nextVar, disposerVar)
      return true
    }
    const fieldAccess = args.fieldAccess ?? BOXED_FIELD_ACCESS
    const childStoreArg = fieldAccess.childMountStoreArg(mountedChild)
    if (childStoreArg === null) return false
    args.lines.push(`  auto ${varName} = mount_${sanitizeCppIdentifier(mountedChild.exportName)}(${childStoreArg}, ${disposerVar});`)
    return true
  }
  // Same rule as the main mount path: a reactive child cannot be inlined.
  if (mountedChild.reactiveState) return false
  // Same wrapper-resolution as the main mount-inlining path: a wrapper
  // component used here (e.g. inside a conditional branch) must be peeled so
  // the wrapped element receives the bindings directly, not a stray
  // display:contents span.
  const resolvedChild = resolveTransparentWrappers(mountedChild, args.components, childProps, args.canMount)
  const effectiveChild = resolvedChild.component
  const effectiveProps = resolvedChild.propBindings
  const mountedRoot = parseTemplateRoot(effectiveChild.template.html)
  const createMountedRoot = mountedRoot ? uiCreateNodeExpression(mountedRoot) : null
  if (!mountedRoot || !createMountedRoot) return false
  args.lines.push(`  auto ${varName} = ${createMountedRoot};`)
  emitInputButtonValueText(args.lines, varName, mountedRoot, args.nextVar)
  return emitTemplateElement({
    component: effectiveChild,
    node: mountedRoot,
    varName,
    components: args.components,
    storeFields: args.storeFields,
    storeMethods: args.storeMethods,
    lines: args.lines,
    nextVar: args.nextVar,
    canMount: args.canMount,
    constants: args.constants,
    propBindings: effectiveProps,
    fieldAccess: args.fieldAccess,
    storeArrayFields: args.storeArrayFields,
    disposerVar,
  })
}

function emitStaticKeyedList(args: {
  component: GeaIrComponent
  components: GeaIrComponent[]
  storeFields: StoreFieldPlan[]
  storeArrayFields: StoreArrayFieldPlan[]
  storeMethods: StoreMethodPlan[]
  constants: ConstantMap
  propBindings: Map<string, PropBinding>
  fieldAccess?: FieldAccess
  lines: string[]
  parentVar: string
  slot: GeaIrSlot
  nextVar: () => string
  canMount: (component: GeaIrComponent) => boolean
  disposerVar?: string
}): boolean {
  const { lines, parentVar, slot, nextVar } = args
  const payload = keyedListPayload(slot)
  const values = staticStringArrayValues(slot.expr)
  const itemParam = payload?.itemParam
  const rowRoot = payload?.rowTemplate ? parseTemplateRoot(payload.rowTemplate.html) : null
  const createRow = rowRoot ? uiCreateNodeExpression(rowRoot) : null
  if (!payload?.rowTemplate || !itemParam || !rowRoot || !createRow) return false

  const objectValues = objectArrayConstant(slot.expr, args.constants)
  if (objectValues) return emitStaticObjectKeyedList(args, payload.rowTemplate, itemParam, objectValues)
  if (!values) return false

  for (const value of values) {
    const rowVar = nextVar()
    lines.push(`  auto ${rowVar} = ${createRow};`)
    lines.push(...attributeLines(rowVar, { tag: rowRoot.tag, className: rowRoot.className }))
    lines.push(...intrinsicStyleLines(rowVar, rowRoot))
    for (const rowSlot of payload.rowTemplate.slots) {
      if (!emitStaticKeyedListRowSlot(lines, rowVar, rowSlot, itemParam, value)) return false
    }
    lines.push(`  ${parentVar}.appendChild(${rowVar});`)
  }
  return true
}

function emitStaticObjectKeyedList(
  args: {
    component: GeaIrComponent
    components: GeaIrComponent[]
  storeFields: StoreFieldPlan[]
  storeArrayFields: StoreArrayFieldPlan[]
  storeMethods: StoreMethodPlan[]
    constants: ConstantMap
    propBindings: Map<string, PropBinding>
    fieldAccess?: FieldAccess
    lines: string[]
    parentVar: string
    slot: GeaIrSlot
    nextVar: () => string
    canMount: (component: GeaIrComponent) => boolean
    disposerVar?: string
  },
  rowTemplate: GeaIrTemplate,
  itemParam: string,
  items: GeaIrConstantObjectArrayItem[],
): boolean {
  for (let index = 0; index < items.length; index += 1) {
    const rewritten = rewriteStaticObjectRowTemplate(rowTemplate, itemParam, items[index])
    const rowRoot = parseTemplateRoot(rewritten.html)
    const createRow = rowRoot ? uiCreateNodeExpression(rowRoot) : null
    if (!rowRoot || !createRow) return false
    const rowVar = args.nextVar()
    const rowComponent: GeaIrComponent = {
      ...args.component,
      id: `${args.component.id}#static-list-${args.slot.index}-${index}`,
      exportName: `${args.component.exportName}_StaticListRow_${args.slot.index}_${index}`,
      template: rewritten,
    }
    args.lines.push(`  auto ${rowVar} = ${createRow};`)
    args.lines.push(`  ${args.parentVar}.appendChild(${rowVar});`)
    if (
      !emitTemplateElement({
        component: rowComponent,
        node: rowRoot,
        varName: rowVar,
        components: args.components,
        storeFields: args.storeFields,
        lines: args.lines,
        nextVar: args.nextVar,
        canMount: args.canMount,
        constants: args.constants,
        propBindings: args.propBindings,
        storeMethods: args.storeMethods,
        fieldAccess: args.fieldAccess,
        storeArrayFields: args.storeArrayFields,
        disposerVar: activeDisposerVar(args),
      })
    ) return false
  }
  return true
}

function emitStaticKeyedListRowSlot(
  lines: string[],
  rowVar: string,
  rowSlot: GeaIrSlot,
  itemParam: string,
  value: string,
): boolean {
  if (!isStaticKeyedListRowSlot(rowSlot, itemParam)) return false
  if (rowSlot.kind === 'class') {
    lines.push(`  ${rowVar}.classList().set(${cppString(value)});`)
    return true
  }
  if (rowSlot.kind === 'text') {
    lines.push(`  ${rowVar}.setText(${cppString(value)});`)
    return true
  }
  return false
}

function canEmitStaticKeyedList(args: {
  component: GeaIrComponent
  slot: GeaIrSlot
  components: GeaIrComponent[]
  storeFields: StoreFieldPlan[]
  storeArrayFields: StoreArrayFieldPlan[]
  constants: ConstantMap
  canMount: (component: GeaIrComponent) => boolean
  propBindings: Map<string, PropBinding>
  storeMethods: StoreMethodPlan[]
}): boolean {
  const { slot } = args
  const payload = keyedListPayload(slot)
  const values = staticStringArrayValues(slot.expr)
  const itemParam = payload?.itemParam
  const rowRoot = payload?.rowTemplate ? parseTemplateRoot(payload.rowTemplate.html) : null
  if (!payload?.rowTemplate || !itemParam) return false
  const objectValues = objectArrayConstant(slot.expr, args.constants)
  if (objectValues) {
    return objectValues.every((item, index) => {
      const rewritten = rewriteStaticObjectRowTemplate(payload.rowTemplate!, itemParam, item)
      const rowComponent: GeaIrComponent = {
        ...args.component,
        id: `${args.component.id}#static-list-${slot.index}-${index}`,
        exportName: `${args.component.exportName}_StaticListRow_${slot.index}_${index}`,
        template: rewritten,
      }
      return canEmitTemplateElement(
        rowComponent,
        args.components,
        args.storeFields,
        args.constants,
        args.canMount,
        args.propBindings,
        args.storeMethods,
        args.storeArrayFields,
      )
    })
  }
  if (!values || !rowRoot || !uiCreateNodeExpression(rowRoot)) return false
  return payload.rowTemplate.slots.every((rowSlot) => isStaticKeyedListRowSlot(rowSlot, itemParam))
}

function isStaticKeyedListSource(slot: GeaIrSlot, constants: ConstantMap): boolean {
  const payload = keyedListPayload(slot)
  if (!payload?.rowTemplate || !payload.itemParam) return false
  return staticStringArrayValues(slot.expr) !== null || objectArrayConstant(slot.expr, constants) !== null
}

function canEmitDynamicKeyedListSlot(args: {
  component: GeaIrComponent
  slot: GeaIrSlot
  components: GeaIrComponent[]
  storeFields: StoreFieldPlan[]
  storeArrayFields: StoreArrayFieldPlan[]
  constants: ConstantMap
  propBindings: Map<string, PropBinding>
  storeMethods: StoreMethodPlan[]
}): boolean {
  return dynamicKeyedListPlan(args) !== null
}

function emitDynamicKeyedListSlot(args: {
  component: GeaIrComponent
  slot: GeaIrSlot
  target: string
  insertBefore?: string
  components: GeaIrComponent[]
  storeFields: StoreFieldPlan[]
  storeArrayFields: StoreArrayFieldPlan[]
  constants: ConstantMap
  propBindings: Map<string, PropBinding>
  storeMethods: StoreMethodPlan[]
  lines: string[]
  disposerVar?: string
}): boolean {
  const plan = dynamicKeyedListPlan(args)
  if (!plan) return false
  const disposerVar = activeDisposerVar(args)
  const suffix = args.slot.index
  const storeVar = `__gea_kl_store_${suffix}`
  const rowsVar = `__gea_kl_rows_${suffix}`
  const rebuildVar = `__gea_kl_rebuild_${suffix}`
  const itemName = plan.itemName
  const itemSource = plan.field.isGetter ? `${storeVar}->${plan.field.fieldName}()` : `${storeVar}->${plan.field.fieldName}`
  const rebuildCaptures = [args.target, storeVar, rowsVar]
  if (args.insertBefore) rebuildCaptures.push(args.insertBefore)
  args.lines.push(`  auto ${storeVar} = ${plan.storeExpr};`)
  args.lines.push(`  auto ${rowsVar} = std::make_shared<std::vector<gea::embedded::ui::NodeHandle>>();`)
  args.lines.push(`  auto ${rebuildVar} = [${rebuildCaptures.join(', ')}]() mutable -> void {`)
  args.lines.push(`    for (auto &__gea_kl_old : *${rowsVar}) __gea_kl_old.remove();`)
  args.lines.push(`    ${rowsVar}->clear();`)
  args.lines.push(`    if (!${storeVar}) return;`)
  args.lines.push(`    const auto &typed_store = ${storeVar};`)
  // Also alias `store` so row-scalar readers emitted as `read_X(store)`
  // (lowerRowExpressionNode) resolve here — this lambda captures the keyed-list
  // store as ${storeVar}, not `store`. Other rebuild paths capture `store`
  // directly, so the reader codegen stays `(store)` and works in both contexts.
  args.lines.push(`    const auto &store = ${storeVar};`)
  args.lines.push(`    const auto &__gea_items = ${itemSource};`)
  args.lines.push('    for (std::size_t __gea_index = 0; __gea_index < __gea_items.size(); ++__gea_index) {')
  args.lines.push(`      const auto &${itemName} = __gea_items[__gea_index];`)
  args.lines.push(`      auto __gea_row_root = ${plan.createRow};`)
  for (const line of plan.rowAttributeLines) args.lines.push(`      ${line}`)
  for (const line of plan.rowSetupLines) args.lines.push(`      ${line.trimStart()}`)
  for (const line of plan.rowTreeLines) args.lines.push(`      ${line}`)
  for (const line of plan.rowSlotLines) args.lines.push(`      ${line}`)
  for (const line of plan.rowEventLines) args.lines.push(`      ${line}`)
  if (args.insertBefore) args.lines.push(`      ${args.target}.insertBefore(__gea_row_root, ${args.insertBefore});`)
  else args.lines.push(`      ${args.target}.appendChild(__gea_row_root);`)
  args.lines.push(`      ${rowsVar}->push_back(__gea_row_root);`)
  args.lines.push('    }')
  args.lines.push('  };')
  const deps = plan.deps.map((dep) => cppString(dep)).join(', ')
  const apply = `[${rebuildVar}]() mutable -> void { ${rebuildVar}(); }`
  if (storeHubEmissionEnabled() && plan.field.storeRuntimeBase === 'compiled' && !plan.field.storeIsSelfStore) {
    args.lines.push(`  gea_rc_bind_hub_apply(${storeVar}, ${disposerVar}, {${deps}}, std::function<void()>(${apply}));`)
  } else {
    args.lines.push(`  bindReactiveApply(gea_cpp_key(${storeVar}), ${disposerVar}, {${deps}}, ${apply});`)
  }
  return true
}

interface DynamicKeyedListPlan {
  field: StoreArrayFieldPlan
  itemName: string
  storeExpr: string
  deps: string[]
  createRow: string
  rowAttributeLines: string[]
  rowSetupLines: string[]
  rowTreeLines: string[]
  rowSlotLines: string[]
  rowEventLines: string[]
}

function dynamicKeyedListPlan(args: {
  component: GeaIrComponent
  slot: GeaIrSlot
  components: GeaIrComponent[]
  storeFields: StoreFieldPlan[]
  storeArrayFields: StoreArrayFieldPlan[]
  constants: ConstantMap
  propBindings: Map<string, PropBinding>
  storeMethods: StoreMethodPlan[]
}): DynamicKeyedListPlan | null {
  const payload = keyedListPayload(args.slot)
  if (!payload?.rowTemplate || !payload.itemParam) return null
  const field = storeArrayFieldForSlot(args.slot, args.storeArrayFields)
  const storeGlobalName = keyedListReceiver(args.slot) ?? field?.storeGlobalName ?? null
  if (!field || !storeGlobalName) return null
  const itemName = sanitizeCppIdentifier(payload.itemParam)
  const rowTemplate = dynamicKeyedListRowTemplate(payload.rowTemplate, args.components, itemName)
  const rowRoot = rowTemplate ? parseTemplateRoot(rowTemplate.html) : null
  const createRow = rowRoot ? uiCreateNodeExpression(rowRoot) : null
  if (!rowTemplate || !rowRoot || !createRow) return null
  const rowSetupSlots = rowTemplate.slots.filter(
    (s) =>
      s.kind !== 'event' &&
      s.kind !== 'class' &&
      s.kind !== 'text' &&
      s.kind !== 'style' &&
      // `key` belongs to keyed reconciliation, never to the rendered node.
      !(s.kind === 'attr' && slotAttrName(s) === 'key') &&
      !(s.kind === 'attr' && slotAttrName(s) === 'src' && !!s.expr && stringLiteralValue(s.expr) === null),
  )
  const rowSetupLines = rootSetupLines('__gea_row_root', rowSetupSlots, [], [], undefined, undefined, args.constants)
  if (!rowSetupLines) return null
  const rowTree = emitDynamicKeyedRowTree(rowRoot, '__gea_row_root', rowTemplate.slots)
  if (!rowTree.ok) return null
  const rowSlots = lowerRowSlots(
    rowTemplate.slots,
    itemName,
    field,
    args.constants,
    rowTree.pathVars,
    args.storeFields,
    args.storeMethods,
  )
  if (rowSlots.lines.length === 0 && rowTree.lines.length === 0 && rowSlots.eventLines.length === 0) return null
  const deps = uniqueStrings([...(field.isGetter ? field.reactiveDeps ?? [] : [field.fieldName]), ...rowSlots.storeFields.map((storeField) => storeField.fieldName)])
  return {
    field,
    itemName,
    storeExpr: `__gea_global_${sanitizeCppIdentifier(storeGlobalName)}()`,
    deps,
    createRow,
    rowAttributeLines: attributeLines('__gea_row_root', { tag: rowRoot.tag, className: rowRoot.className, attrs: rowRoot.attrs }, rowTemplate.slots),
    rowSetupLines,
    rowTreeLines: rowTree.lines,
    rowSlotLines: rowSlots.lines,
    rowEventLines: rowSlots.eventLines,
  }
}

function storeArrayFieldForSlot(slot: GeaIrSlot, fields: StoreArrayFieldPlan[]): StoreArrayFieldPlan | null {
  const fieldName = fieldNameForSlot(slot)
  if (!fieldName) return null
  const receiver = keyedListReceiver(slot)
  const matches = fields.filter((field) => field.fieldName === fieldName)
  if (receiver) {
    const byReceiver = matches.filter((field) => field.storeGlobalName === receiver)
    if (byReceiver.length === 1) return byReceiver[0]
  }
  return matches.length === 1 ? matches[0] : null
}

function keyedListReceiver(slot: GeaIrSlot): string | null {
  if (slot.exprPath && slot.exprPath.length > 1) return sanitizeCppIdentifier(slot.exprPath[0])
  const match = slot.expr?.trim().match(/^([A-Za-z_$][A-Za-z0-9_$]*)\./)
  return match ? sanitizeCppIdentifier(match[1]) : null
}

function dynamicKeyedListRowTemplate(
  rowTemplate: GeaIrTemplate,
  components: GeaIrComponent[],
  itemName: string,
): GeaIrTemplate | null {
  if (parseTemplateRoot(rowTemplate.html)) return rowTemplate
  const mountSlots = rowTemplate.slots.filter((slot) => slot.kind === 'mount')
  if (mountSlots.length !== 1) return null
  const childTag = mountSlotTag(mountSlots[0])
  const child = childTag ? components.find((component) => component.exportName === childTag) : null
  if (!child || !parseTemplateRoot(child.template.html)) return null
  const bindings = rowComponentPropBindings(mountSlots[0], itemName)
  if (!bindings) return null
  return {
    html: child.template.html,
    slots: child.template.slots.map((slot) => rewriteRowComponentSlot(slot, bindings)),
  }
}

function rowComponentPropBindings(slot: GeaIrSlot, itemName: string): ReadonlyMap<string, string> | null {
  const attrs = mountSlotAttrs(slot)
  if (attrs.length === 0) return null
  const bindings = new Map<string, string>()
  let bindsItem = false
  for (const attr of attrs) {
    const parsed = parseMountAttr(attr)
    if (!parsed) return null
    // `key` belongs to the list reconciler, not to the child component.
    if (parsed.name === 'key') continue
    bindings.set(parsed.name, parsed.expr)
    if (/^[A-Za-z_$][A-Za-z0-9_$]*$/.test(parsed.expr) && sanitizeCppIdentifier(parsed.expr) === itemName) {
      bindsItem = true
    }
  }
  return bindsItem ? bindings : null
}

function rewriteRowComponentSlot(slot: GeaIrSlot, bindings: ReadonlyMap<string, string>): GeaIrSlot {
  return {
    ...slot,
    ...(slot.expr ? { expr: rewriteRowComponentExpression(slot.expr, bindings) } : {}),
    ...(slot.exprObjectFields
      ? {
          exprObjectFields: slot.exprObjectFields.map((field) => ({
            ...field,
            expr: rewriteRowComponentExpression(field.expr, bindings),
          })),
        }
      : {}),
  }
}

function rewriteRowComponentExpression(expr: string, bindings: ReadonlyMap<string, string>): string {
  let rewritten = expr
  // Longest names first keeps `props.bookTitle` independent from `props.book`.
  const entries = [...bindings.entries()].sort(([a], [b]) => b.length - a.length)
  for (const [propName, value] of entries) {
    const prop = escapeRegExp(propName)
    const replacement = /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(value) ? value : `(${value})`
    rewritten = rewritten
      .replace(new RegExp(`\\bthis\\.props\\.${prop}\\b`, 'g'), replacement)
      .replace(new RegExp(`\\bprops\\.${prop}\\b`, 'g'), replacement)
  }
  return rewritten
}

function emitDynamicKeyedRowTree(root: TemplateElement, rowVarName: string, slots: GeaIrSlot[]): {
  ok: boolean
  lines: string[]
  pathVars: Map<string, string>
} {
  const lines: string[] = []
  const pathVars = new Map<string, string>([['', rowVarName]])
  const dynamicTextPaths = new Set(slots.filter((slot) => slot.kind === 'text').map((slot) => slot.walk.join(',')))
  let nextId = 0
  const nextVar = () => `__gea_kl_row_${nextId++}`
  const visit = (parent: TemplateElement, parentVar: string, parentPath: number[]): boolean => {
    for (let index = 0; index < parent.children.length; index += 1) {
      const child = parent.children[index]
      const childPath = [...parentPath, index]
      if (child.kind === 'element') {
        const createChild = uiCreateNodeExpression(child)
        if (!createChild) return false
        const childVar = nextVar()
        lines.push(`auto ${childVar} = ${createChild};`)
        lines.push(`${childVar}.setTagName(${cppString(child.tag.toLowerCase())});`)
        if (child.className) lines.push(`${childVar}.classList().set(${cppString(child.className)});`)
        for (const [name, value] of Object.entries(child.attrs ?? {})) {
          const lower = name.toLowerCase()
          if (lower === 'class' || lower === 'style') continue
          lines.push(`${childVar}.setAttribute(${cppString(name)}, ${cppString(value)});`)
        }
        lines.push(`${parentVar}.appendChild(${childVar});`)
        pathVars.set(childPath.join(','), childVar)
        if (!visit(child, childVar, childPath)) return false
        continue
      }
      if (child.kind === 'text') {
        const text = renderableTemplateText(child.text)
        if (!text) continue
        // The IR uses a literal `0` text node as the DOM anchor for a dynamic
        // JSX expression. A real authored zero has the same HTML spelling, so
        // only treat it as an anchor when a text slot targets this exact path.
        // Previously every literal zero in a keyed row was discarded.
        if (text === '0' && dynamicTextPaths.has(childPath.join(','))) {
          pathVars.set(childPath.join(','), parentVar)
          continue
        }
        if (canUseTextNodeForElement(parent)) {
          lines.push(`${parentVar}.setText(${cppString(text)});`)
          pathVars.set(childPath.join(','), parentVar)
        } else {
          const textVar = nextVar()
          lines.push(`auto ${textVar} = gea::embedded::ui::Document::instance().createText();`)
          lines.push(`${textVar}.setText(${cppString(text)});`)
          lines.push(`${parentVar}.appendChild(${textVar});`)
          pathVars.set(childPath.join(','), textVar)
        }
        continue
      }
      if (dynamicTextPaths.has(childPath.join(',')) && !canUseTextNodeForElement(parent)) {
        // A mixed inline run is encoded as `<!--slot-->suffix`. The parent must
        // remain a View so its runs lay out side by side; give the dynamic slot
        // its own text node instead of calling setText on that View (which made
        // numeric 0, and every other prefix, disappear before `% READ`).
        const textVar = nextVar()
        lines.push(`auto ${textVar} = gea::embedded::ui::Document::instance().createText();`)
        lines.push(`${parentVar}.appendChild(${textVar});`)
        pathVars.set(childPath.join(','), textVar)
        continue
      }
      pathVars.set(childPath.join(','), parentVar)
    }
    return true
  }
  return visit(root, rowVarName, []) ? { ok: true, lines, pathVars } : { ok: false, lines: [], pathVars }
}

function rewriteStaticObjectRowTemplate(
  template: GeaIrTemplate,
  itemParam: string,
  item: GeaIrConstantObjectArrayItem,
): GeaIrTemplate {
  return {
    html: template.html,
    slots: template.slots.map((slot) => rewriteStaticObjectRowSlot(slot, itemParam, item)),
  }
}

function rewriteStaticObjectRowSlot(
  slot: GeaIrSlot,
  itemParam: string,
  item: GeaIrConstantObjectArrayItem,
): GeaIrSlot {
  return {
    ...slot,
    ...(slot.expr ? { expr: rewriteStaticObjectRowExpression(slot.expr, itemParam, item) } : {}),
    ...(slot.exprObjectFields
      ? {
          exprObjectFields: slot.exprObjectFields.map((field) => ({
            ...field,
            expr: rewriteStaticObjectRowExpression(field.expr, itemParam, item),
          })),
        }
      : {}),
    ...(slot.payload ? { payload: rewriteStaticObjectRowPayload(slot.payload, itemParam, item) } : {}),
  }
}

function rewriteStaticObjectRowPayload(payload: unknown, itemParam: string, item: GeaIrConstantObjectArrayItem): unknown {
  if (!payload || typeof payload !== 'object') return payload
  if (!('attrs' in payload) || !Array.isArray((payload as { attrs?: unknown }).attrs)) return payload
  return {
    ...(payload as Record<string, unknown>),
    attrs: (payload as { attrs: unknown[] }).attrs.map((attr) => {
      if (!attr || typeof attr !== 'object' || typeof (attr as { code?: unknown }).code !== 'string') return attr
      return {
        ...(attr as Record<string, unknown>),
        code: rewriteStaticObjectRowExpression((attr as { code: string }).code, itemParam, item),
      }
    }),
  }
}

function rewriteStaticObjectRowExpression(
  expr: string,
  itemParam: string,
  item: GeaIrConstantObjectArrayItem,
): string {
  let rewritten = expr
  for (const field of item.fields) {
    const escapedItem = escapeRegExp(itemParam)
    const escapedField = escapeRegExp(field.name)
    rewritten = rewritten.replace(
      new RegExp(`\\b${escapedItem}\\.${escapedField}\\b`, 'g'),
      staticObjectFieldLiteral(field),
    )
  }
  return rewritten
}

function staticObjectFieldLiteral(field: GeaIrConstantObjectField): string {
  if (field.valueType === 'string') return JSON.stringify(field.value)
  if (field.valueType === 'number' || field.valueType === 'boolean') return field.value
  return 'null'
}

function isStaticKeyedListRowSlot(slot: GeaIrSlot, itemParam: string): boolean {
  if (slot.walk.length !== 0) return false
  if (slot.kind !== 'class' && slot.kind !== 'text') return false
  if (slot.exprPath && slot.exprPath.length === 1 && slot.exprPath[0] === itemParam) return true
  return slot.expr?.trim() === itemParam
}

function keyedListPayload(slot: GeaIrSlot): { itemParam?: string; rowTemplate?: { html: string; slots: GeaIrSlot[] } } | null {
  const payload = slot.payload
  if (!payload || typeof payload !== 'object') return null
  const record = payload as { itemParam?: unknown; rowTemplate?: unknown }
  const itemParam = typeof record.itemParam === 'string' ? record.itemParam : undefined
  const rowTemplate =
    record.rowTemplate && typeof record.rowTemplate === 'object'
      ? (record.rowTemplate as { html?: unknown; slots?: unknown })
      : null
  if (!rowTemplate || typeof rowTemplate.html !== 'string' || !Array.isArray(rowTemplate.slots)) return { itemParam }
  return { itemParam, rowTemplate: { html: rowTemplate.html, slots: rowTemplate.slots as GeaIrSlot[] } }
}

// ── typed keyed list (self-store ReactiveComponent over a primitive array) ─────
//
// Renders `{this.cells.map((cell, index) => <row…/>)}` for a typed self-store:
// the rows are rebuilt from the live `store-><field>` vector whenever the
// companion `<field>__rev` Signal ticks (any element write — see
// gea_rc_vector_set_notify). Row events install ONE delegated listener per event
// type that resolves the row index by hit-testing the kept row handles, then
// calls the typed method (`store->play(index)`). Rebuild-all is O(rows) — right
// for the small lists an embedded screen renders; granular per-row patching can
// come later behind the same seam.

interface RowExpr {
  cpp: string
  kind: 'string' | 'number' | 'boolean'
  styleUnit?: 'percent' | 'color' | 'keyword' | 'px'
}

function rowExprAsString(expr: RowExpr): string {
  return expr.kind === 'string' ? expr.cpp : `gea_cpp_to_string(${expr.cpp})`
}

function rowExprAsBoolean(expr: RowExpr): string {
  if (expr.kind === 'boolean') return expr.cpp
  if (expr.kind === 'number') return `(${expr.cpp} != static_cast<double>(0))`
  return `(!${expr.cpp}.empty())`
}

// Lower a row-template expression over the map callback's item/index params and
// literals. Returns null on anything richer — the component then falls off the
// typed fast path loudly (no renderer) instead of emitting broken code.
function lowerRowExpr(
  expr: string | undefined,
  itemParam: string,
  indexParam: string | undefined,
  itemKind: RowExpr['kind'],
  constants: ConstantMap = new Map(),
): RowExpr | null {
  if (!expr) return null
  const text = stripOuterParens(expr.trim())
  const ternary = splitTopLevelTernary(text)
  if (ternary) {
    const test = lowerRowExpr(ternary.test, itemParam, indexParam, itemKind, constants)
    const consequent = lowerRowExpr(ternary.consequent, itemParam, indexParam, itemKind, constants)
    const alternate = lowerRowExpr(ternary.alternate, itemParam, indexParam, itemKind, constants)
    if (!test || !consequent || !alternate) return null
    const kind = consequent.kind === 'string' || alternate.kind === 'string' ? 'string' : consequent.kind
    const branch = (value: RowExpr) => (kind === 'string' ? rowExprAsString(value) : value.cpp)
    return { cpp: `(${test.cpp} ? ${branch(consequent)} : ${branch(alternate)})`, kind }
  }
  const binary = splitTopLevelBinary(text)
  if (binary) {
    const left = lowerRowExpr(binary.left, itemParam, indexParam, itemKind, constants)
    const right = lowerRowExpr(binary.right, itemParam, indexParam, itemKind, constants)
    if (!left || !right) return null
    return { cpp: `(${left.cpp} ${cppBinaryOperator(binary.op)} ${right.cpp})`, kind: 'boolean' }
  }
  const plusParts = splitTopLevelOperator(text, '+')
  if (plusParts.length > 1) {
    const parts = plusParts.map((part) => lowerRowExpr(part, itemParam, indexParam, itemKind, constants))
    if (parts.some((part) => !part)) return null
    const lowered = parts as RowExpr[]
    if (lowered.every((part) => part.kind === 'number')) {
      return { cpp: `(${lowered.map((part) => part.cpp).join(' + ')})`, kind: 'number' }
    }
    return { cpp: `(${lowered.map(rowExprAsString).join(' + ')})`, kind: 'string' }
  }
  const string = stringLiteralValue(text)
  if (string !== null) return { cpp: `std::string(${cppString(string)})`, kind: 'string' }
  const constant = constants.get(text)
  if (constant?.valueType === 'string') return { cpp: `std::string(${cppString(constant.value)})`, kind: 'string' }
  if (constant?.valueType === 'number') return { cpp: `static_cast<double>(${constant.value})`, kind: 'number' }
  if (constant?.valueType === 'boolean') return { cpp: constant.value, kind: 'boolean' }
  if (numericLiteralValue(text) !== null) return { cpp: `static_cast<double>(${text})`, kind: 'number' }
  if (text === 'true' || text === 'false') return { cpp: text, kind: 'boolean' }
  if (text === itemParam) return { cpp: '__gea_item', kind: itemKind }
  if (indexParam && text === indexParam) return { cpp: 'static_cast<double>(__gea_index)', kind: 'number' }
  return null
}

function lowerRowPxLengthStyleExpr(
  expr: string | undefined,
  propertyName: string,
  itemParam: string,
  indexParam: string | undefined,
  itemKind: RowExpr['kind'],
  constants: ConstantMap = new Map(),
): RowExpr | null {
  if (!expr || !isPxLengthStylePropertyName(propertyName)) return null
  const parts = parseTemplateLiteralParts(stripOuterParens(expr.trim()))
  if (!parts || parts.length !== 2) return null
  if (parts[0].kind !== 'expr' || parts[1].kind !== 'text' || parts[1].value !== 'px') return null
  const lowered = lowerRowExpr(parts[0].value, itemParam, indexParam, itemKind, constants)
  return lowered ? { cpp: rowExprAsNumber(lowered), kind: 'number', styleUnit: isBorderWidthStylePropertyName(propertyName) ? 'px' : undefined } : null
}

function lowerRowPercentLengthStyleExpr(
  expr: string | undefined,
  propertyName: string,
  itemParam: string,
  indexParam: string | undefined,
  itemKind: RowExpr['kind'],
  constants: ConstantMap = new Map(),
): RowExpr | null {
  if (!expr || !stylePercentPropertyEnumForPropertyName(propertyName)) return null
  const parts = parseTemplateLiteralParts(stripOuterParens(expr.trim()))
  if (!parts || parts.length !== 2) return null
  if (parts[0].kind !== 'expr' || parts[1].kind !== 'text' || parts[1].value !== '%') return null
  const lowered = lowerRowExpr(parts[0].value, itemParam, indexParam, itemKind, constants)
  return lowered ? { cpp: rowExprAsNumber(lowered), kind: 'number', styleUnit: 'percent' } : null
}

function lowerRowRotateStyleExpr(
  expr: string | undefined,
  propertyName: string,
  itemParam: string,
  indexParam: string | undefined,
  itemKind: RowExpr['kind'],
  constants: ConstantMap = new Map(),
): RowExpr | null {
  if (!expr || !isRotateTransformPropertyName(propertyName)) return null
  const parts = parseTemplateLiteralParts(stripOuterParens(expr.trim()))
  const degrees = rotateDegreesTemplateExpression(parts, propertyName)
  if (!degrees) return null
  const lowered = lowerRowExpr(degrees, itemParam, indexParam, itemKind, constants)
  return lowered ? { cpp: rowExprAsNumber(lowered), kind: 'number' } : null
}

function lowerRowOpaqueColorStyleExpr(
  expr: string | undefined,
  propertyName: string,
  itemParam: string,
  indexParam: string | undefined,
  itemKind: RowExpr['kind'],
  constants: ConstantMap = new Map(),
): RowExpr | null {
  if (!expr || !styleOpaqueColorTargetForPropertyName(propertyName)) return null
  const text = stripOuterParens(expr.trim())
  const literal = opaqueHexColorExpressionForStyleValue(text, constants, new Map())
  if (literal) return { cpp: literal, kind: 'number', styleUnit: 'color' }
  const ternary = splitTopLevelTernary(text)
  if (!ternary) return null
  const test = lowerRowExpr(ternary.test, itemParam, indexParam, itemKind, constants)
  const consequent = opaqueHexColorExpressionForStyleValue(ternary.consequent, constants, new Map())
  const alternate = opaqueHexColorExpressionForStyleValue(ternary.alternate, constants, new Map())
  if (!test || !consequent || !alternate) return null
  return {
    cpp: `(${rowExprAsBoolean(test)} ? ${consequent} : ${alternate})`,
    kind: 'number',
    styleUnit: 'color',
  }
}

function lowerRowKeywordStyleExpr(
  expr: string | undefined,
  propertyName: string,
  itemParam: string,
  indexParam: string | undefined,
  itemKind: RowExpr['kind'],
  constants: ConstantMap = new Map(),
): RowExpr | null {
  if (!expr || !styleKeywordPropertiesForPropertyName(propertyName)) return null
  const text = stripOuterParens(expr.trim())
  const literal = keywordCommonValue(keywordEntriesForExpr(propertyName, text, constants, new Map()))
  if (literal !== null) return { cpp: String(literal), kind: 'number', styleUnit: 'keyword' }
  const ternary = splitTopLevelTernary(text)
  if (!ternary) return null
  const test = lowerRowExpr(ternary.test, itemParam, indexParam, itemKind, constants)
  const consequent = keywordEntriesForExpr(propertyName, ternary.consequent, constants, new Map())
  const alternate = keywordEntriesForExpr(propertyName, ternary.alternate, constants, new Map())
  const consequentValue = keywordCommonValue(consequent)
  const alternateValue = keywordCommonValue(alternate)
  if (!test || consequentValue === null || alternateValue === null || !keywordEntriesCompatible(consequent, alternate)) return null
  return {
    cpp: `(${rowExprAsBoolean(test)} ? ${consequentValue} : ${alternateValue})`,
    kind: 'number',
    styleUnit: 'keyword',
  }
}

function rowExprAsNumber(expr: RowExpr): string {
  return expr.kind === 'number' ? expr.cpp : `gea::runtime::coerce::to_number(${expr.cpp})`
}

// The element a row slot targets, as a dotted path of element-child indices
// (mirrors how the main tree keys pathVars). A trailing {child:N} (text slots)
// belongs to the containing element.
function rowSlotElemPath(slot: GeaIrSlot): string {
  const kinds = slot.walkKinds
  if (kinds) return kinds.filter((kind): kind is { elem: number } => 'elem' in kind).map((kind) => kind.elem).join('.')
  const walk = slot.kind === 'text' ? slot.walk.slice(0, -1) : slot.walk
  return walk.join('.')
}

function typedRowEventType(slot: GeaIrSlot): string | null {
  const attrName = slotAttrName(slot)
  if (!attrName) return null
  const map: Record<string, string> = { onclick: 'click' }
  return map[attrName.toLowerCase()] ?? null
}

// Lower `() => this.<method>(<item/index/literal args>)` to a typed call.
function lowerRowEventCall(expr: string | undefined, itemParam: string, indexParam: string | undefined, itemKind: RowExpr['kind']): string | null {
  if (!expr) return null
  const arrow = expr.trim().match(/^\(\s*\)\s*=>\s*([\s\S]+)$/)
  if (!arrow) return null
  let body = arrow[1].trim()
  if (body.startsWith('{') && body.endsWith('}')) body = body.slice(1, -1).trim()
  body = body.replace(/;$/, '').trim()
  const call = body.match(/^this\.([A-Za-z_$][A-Za-z0-9_$]*)\(([\s\S]*)\)$/)
  if (!call || body.includes(';')) return null
  const args = splitTopLevelOperator(call[2], ',').map((arg) => arg.trim()).filter((arg) => arg.length > 0)
  const lowered: string[] = []
  for (const arg of args) {
    const value = lowerRowExpr(arg, itemParam, indexParam, itemKind)
    if (!value) return null
    lowered.push(value.cpp)
  }
  return `(void)store->${sanitizeCppIdentifier(call[1])}(${lowered.join(', ')});`
}

function emitTypedKeyedListSlot(
  lines: string[],
  target: string,
  slot: GeaIrSlot,
  component: GeaIrComponent,
  constants: ConstantMap,
  disposerVar: string,
): boolean {
  const member = (slot.expr ?? '').trim().match(/^this\.([A-Za-z_$][A-Za-z0-9_$]*)$/)
  if (!member) return false
  const arrayField = reactiveArrayFieldInfo(component).find((field: ReactiveArrayFieldInfo) => field.name === member[1])
  if (!arrayField) return false
  const payload = slot.payload as { itemParam?: string; indexParam?: string; rowTemplate?: { html: string; slots: GeaIrSlot[] } } | null
  const itemParam = payload?.itemParam
  const indexParam = payload?.indexParam
  const rowTemplate = payload?.rowTemplate
  if (!itemParam || !rowTemplate) return false
  const rowRoot = parseTemplateRoot(rowTemplate.html)
  if (!rowRoot) return false
  const itemKind: RowExpr['kind'] = arrayField.elementValueType
  const fieldName = sanitizeCppIdentifier(arrayField.name)
  const rev = arrayRevFieldName(arrayField.name)

  // Group row slots by the element they target; collect root events separately.
  const slotsByPath = new Map<string, GeaIrSlot[]>()
  const rootEvents: Array<{ type: string; call: string }> = []
  for (const rowSlot of rowTemplate.slots) {
    if (rowSlot.kind === 'event') {
      if (rowSlotElemPath(rowSlot) !== '') return false // events on row root only (v1)
      const type = typedRowEventType(rowSlot)
      const call = lowerRowEventCall(rowSlot.expr, itemParam, indexParam, itemKind)
      if (!type || !call) return false
      rootEvents.push({ type, call })
      continue
    }
    if (rowSlot.kind !== 'style' && rowSlot.kind !== 'text' && rowSlot.kind !== 'class') return false
    const key = rowSlotElemPath(rowSlot)
    const bucket = slotsByPath.get(key) ?? []
    bucket.push(rowSlot)
    slotsByPath.set(key, bucket)
  }

  // Build the row subtree (recursive over elements; bounded row shapes).
  const rowLines: string[] = []
  let rowVarCount = 0
  const nextRowVar = () => `__gea_kl_n${rowVarCount++}`
  const emitRowElement = (node: TemplateElement, varName: string, elemPath: string): boolean => {
    rowLines.push(`auto ${varName} = ${uiCreateNodeExpression(node) ?? 'gea::embedded::ui::Document::instance().createView()'};`)
    rowLines.push(`${varName}.setTagName(${cppString(node.tag.toLowerCase())});`)
    if (node.className) rowLines.push(`${varName}.classList().set(${cppString(node.className)});`)
    const elementSlots = slotsByPath.get(elemPath) ?? []
    const hasDynamicTextSlot = elementSlots.some((rowSlot) => rowSlot.kind === 'text')
    for (const rowSlot of elementSlots) {
      if (rowSlot.kind === 'style') {
        for (const styleField of rowSlot.exprObjectFields ?? []) {
          const property = cssPropertyName(styleField.name)
          const lowered =
            lowerRowRotateStyleExpr(styleField.expr, property, itemParam, indexParam, itemKind, constants) ??
            lowerRowPxLengthStyleExpr(styleField.expr, property, itemParam, indexParam, itemKind, constants) ??
            lowerRowPercentLengthStyleExpr(styleField.expr, property, itemParam, indexParam, itemKind, constants) ??
            lowerRowOpaqueColorStyleExpr(styleField.expr, property, itemParam, indexParam, itemKind, constants) ??
            lowerRowKeywordStyleExpr(styleField.expr, property, itemParam, indexParam, itemKind, constants) ??
            lowerRowExpr(styleField.expr, itemParam, indexParam, itemKind, constants)
          if (!lowered) return false
          const propertyArg = stylePropertyArgument(property)
          const literalString = stringLiteralValue(styleField.expr)
          const directStringLines = literalString !== null ? directStringStyleApplyLines(varName, property, literalString) : null
          const numberLine =
            lowered.kind === 'number'
              ? styleNumberApplyLine(varName, property, lowered.cpp, lowered.styleUnit) ??
                `gea::embedded::ui::StyleSheet::instance().applyNumberProperty(${varName}, ${propertyArg}, static_cast<double>(${lowered.cpp}));`
              : null
          if (directStringLines) rowLines.push(...directStringLines)
          else rowLines.push(
            lowered.kind === 'number'
              ? numberLine!
              : `gea::embedded::ui::StyleSheet::instance().applyProperty(${varName}, ${propertyArg}, ${rowExprAsString(lowered)});`,
          )
        }
        continue
      }
      if (rowSlot.kind === 'class') {
        const lowered = lowerRowExpr(rowSlot.expr, itemParam, indexParam, itemKind, constants)
        if (!lowered) return false
        rowLines.push(`${varName}.classList().set(${rowExprAsString(lowered)});`)
        continue
      }
      // text
      const lowered = lowerRowExpr(rowSlot.expr, itemParam, indexParam, itemKind, constants)
      if (!lowered) return false
      rowLines.push(`${varName}.setText((${rowExprAsString(lowered)}).c_str());`)
    }
    let elemChildIndex = 0
    for (const child of node.children) {
      if (child.kind === 'element') {
        const childVar = nextRowVar()
        if (!emitRowElement(child, childVar, elemPath === '' ? String(elemChildIndex) : `${elemPath}.${elemChildIndex}`)) return false
        rowLines.push(`${varName}.appendChild(${childVar});`)
        elemChildIndex += 1
        continue
      }
      if (child.kind === 'text') {
        // A zero is an IR placeholder only when this element actually owns a
        // dynamic text slot. Otherwise it is authored content and must render.
        const text = renderableTemplateText(child.text)
        if (text && (text !== '0' || !hasDynamicTextSlot)) rowLines.push(`${varName}.setText(${cppString(text)});`)
      }
    }
    return true
  }
  if (!emitRowElement(rowRoot, '__gea_kl_row', '')) return false

  const rows = `__gea_kl_rows_${slot.index}`
  const rebuild = `__gea_kl_rebuild_${slot.index}`
  lines.push(`  auto ${rows} = std::make_shared<std::vector<gea::embedded::ui::NodeHandle>>();`)
  lines.push(`  auto ${rebuild} = [${target}, store, ${rows}]() mutable -> void {`)
  lines.push(`    for (auto &__gea_kl_old : *${rows}) __gea_kl_old.remove();`)
  lines.push(`    ${rows}->clear();`)
  lines.push(`    const auto &__gea_items = store->${fieldName};`)
  lines.push(`    for (std::size_t __gea_index = 0; __gea_index < __gea_items.size(); ++__gea_index) {`)
  lines.push(`      const auto &__gea_item = __gea_items[__gea_index];`)
  lines.push(`      (void)__gea_item;`)
  for (const line of rowLines) lines.push(`      ${line}`)
  lines.push(`      ${target}.appendChild(__gea_kl_row);`)
  lines.push(`      ${rows}->push_back(__gea_kl_row);`)
  lines.push(`    }`)
  lines.push(`  };`)
  lines.push(`  ${rebuild}();`)
  lines.push(`  store->${rev}.subscribe(${rebuild});`)
  for (const [eventIndex, event] of rootEvents.entries()) {
    const listenerToken = `__gea_kl_event_listener_${slot.index}_${eventIndex}`
    lines.push(`  auto ${listenerToken} = gea::embedded::ui::Document::instance().body().addEventListener(${cppString(event.type)}, [store, ${rows}](gea::framework::events::PointerEvent &event) mutable {`)
    lines.push(`    for (std::size_t __gea_index = 0; __gea_index < ${rows}->size(); ++__gea_index) {`)
    lines.push(`      if (!gea::embedded::ui::Tree::instance().containsNode((*${rows})[__gea_index].id(), event.targetId)) continue;`)
    lines.push(`      const auto &__gea_item = store->${fieldName}[__gea_index];`)
    lines.push(`      (void)__gea_item;`)
    lines.push(`      ${event.call}`)
    lines.push(`      return;`)
    lines.push(`    }`)
    lines.push(`  });`)
    lines.push(`  if (${disposerVar}) {`)
    lines.push(`    ${disposerVar}->add([${listenerToken}]() -> void {`)
    lines.push(`      gea::embedded::ui::Document::instance().body().removeEventListener(${cppString(event.type)}, ${listenerToken});`)
    lines.push(`    });`)
    lines.push(`  }`)
  }
  return true
}

function staticStringArrayValues(expr: string | undefined): string[] | null {
  if (!expr) return null
  const text = stripOuterParens(expr.trim())
  if (!text.startsWith('[') || !text.endsWith(']')) return null
  const inner = text.slice(1, -1).trim()
  if (!inner) return []
  const values: string[] = []
  for (const part of splitTopLevelOperator(inner, ',')) {
    const value = stringLiteralValue(part)
    if (value === null) return null
    values.push(value)
  }
  return values
}

function resolveRootSetupPropSlots(slots: GeaIrSlot[], propBindings: Map<string, PropBinding>, constants: ConstantMap): GeaIrSlot[] {
  if (propBindings.size === 0) return slots
  let changed = false
  const next = slots.map((slot) => {
    if (!isRootSetupSlot(slot, constants)) return slot
    let expr = slot.expr
    let exprObjectFields = slot.exprObjectFields
    if (expr) {
      const resolved = resolveLiteralProps(expr, propBindings)
      if (resolved !== expr) {
        expr = resolved
        changed = true
      }
    }
    if (exprObjectFields) {
      const fields = exprObjectFields.map((field) => {
        const resolved = resolveLiteralProps(field.expr, propBindings)
        if (resolved === field.expr) return field
        changed = true
        return { ...field, expr: resolved }
      })
      exprObjectFields = fields
    }
    return { ...slot, ...(expr !== slot.expr ? { expr } : {}), ...(exprObjectFields !== slot.exprObjectFields ? { exprObjectFields } : {}) }
  })
  return changed ? next : slots
}

function resolveLiteralProps(expr: string, propBindings: Map<string, PropBinding>): string {
  let out = expr
  for (const name of propBindings.keys()) {
    const value = literalPropExpression(name, propBindings)
    if (value === null) continue
    const escaped = escapeRegExp(name)
    const pattern = new RegExp(`\\bthis\\.props\\.${escaped}\\b|\\bprops\\.${escaped}\\b|(?<!\\.)\\b${escaped}\\b`, 'g')
    out = out.replace(pattern, value)
  }
  return out
}

function literalPropExpression(name: string, propBindings: Map<string, PropBinding>, seenProps = new Set<string>()): string | null {
  const binding = propBindings.get(name)
  if (!binding || seenProps.has(name)) return null
  const expr = stripOuterParens(binding.expr.trim())
  if (stringLiteralValue(expr) !== null || numericLiteralValue(expr) !== null || expr === 'true' || expr === 'false') return expr
  // Arrow function passed as a prop (e.g. `onClick={() => Settings.foo()}`).
  // Wrapper components like `<View>` forward those via `props.onClick`, and
  // `lowerRootEvent` needs an actual arrow expression to parse — substitute
  // the literal directly so the event lowering pipeline sees the same shape
  // it would have if the user wrote the handler inline on the wrapper's
  // root element.
  if (isArrowFunctionExpression(expr)) return expr
  const prop = propNameForExpression(expr)
  if (!prop) return null
  const nextSeen = new Set(seenProps)
  nextSeen.add(name)
  return literalPropExpression(prop, binding.parentProps, nextSeen)
}

function isArrowFunctionExpression(expr: string): boolean {
  // Cheap detection: arrow params either as `()`, `(x)`, `(x, y, …)`, or a
  // single bare identifier — followed by `=>`. Anything more exotic
  // (destructured params, default values) falls through to the next branch
  // and is left to the caller's existing handling.
  return /^(?:\([^)]*\)|[A-Za-z_$][A-Za-z0-9_$]*)\s*=>/.test(expr)
}

function canLowerTemplateClassSlot(
  slot: GeaIrSlot,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
): boolean {
  const classObject = lowerTemplateClassObject(slot.expr, storeFields, constants, propBindings)
  if (classObject) return !classObject.lowered.fields.some((field) => !field.readerName)
  const lowered = lowerTemplateExpression(slot.expr, storeFields, constants, propBindings)
  return !!lowered && !lowered.fields.some((field) => !field.readerName)
}

function emitTemplateClassSlot(
  lines: string[],
  target: string,
  slot: GeaIrSlot,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
  fieldAccess: FieldAccess = BOXED_FIELD_ACCESS,
  disposerVar = 'disposer',
): boolean {
  const classObject = lowerTemplateClassObject(slot.expr, storeFields, constants, propBindings)
  if (classObject) {
    if (classObject.lowered.fields.some((field) => !field.readerName)) return false
    emitReactiveApply(lines, target, classObject.lowered, [
      '{',
      'std::string __gea_class_name;',
      ...classObject.entries.map(
        (entry) =>
          `if (${loweredAsBoolean(entry.condition)}) { if (!__gea_class_name.empty()) __gea_class_name.push_back(' '); __gea_class_name.append(${cppString(entry.className)}); }`,
      ),
      `${target}.classList().set(__gea_class_name);`,
      '}',
    ], fieldAccess, [], disposerVar)
    return true
  }
  const lowered = lowerTemplateExpression(slot.expr, storeFields, constants, propBindings)
  if (!lowered || lowered.fields.some((field) => !field.readerName)) return false
  emitReactiveApply(lines, target, lowered, `${target}.classList().set((${loweredAsString(lowered)}));`, fieldAccess, [], disposerVar)
  return true
}

function lowerTemplateClassObject(
  expr: string | undefined,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
): LoweredClassObject | null {
  if (!expr) return null
  const text = stripOuterParens(expr.trim())
  if (!text.startsWith('{') || !text.endsWith('}')) return null
  const inner = text.slice(1, -1).trim()
  if (!inner) {
    return {
      lowered: { kind: 'boolean', expr: 'true', deps: [], fields: [] },
      entries: [],
    }
  }
  const entries: LoweredClassObject['entries'] = []
  for (const part of splitTopLevelOperator(inner, ',')) {
    if (part.startsWith('...')) return null
    const colon = findTopLevelOperator(part, ':')
    if (colon < 0) return null
    const key = classObjectKeyName(part.slice(0, colon).trim())
    if (!key) return null
    const condition = lowerTemplateExpression(part.slice(colon + 1).trim(), storeFields, constants, propBindings)
    if (!condition) return null
    entries.push({ className: key, condition })
  }
  return {
    lowered: mergeLowered('boolean', 'true', entries.map((entry) => entry.condition)),
    entries,
  }
}

function classObjectKeyName(expr: string): string | null {
  const string = stringLiteralValue(expr)
  if (string !== null) return string
  return /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(expr) ? expr : null
}

function canLowerTemplateStyleSlot(
  slot: GeaIrSlot,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
): boolean {
  const fields = styleObjectFieldsForSlot(slot, propBindings)
  if (!fields || fields.length === 0) return false
  for (const field of fields) {
    const propertyName = cssPropertyName(field.name)
    const lowered =
      lowerPxLengthStyleValue(field.expr, propertyName, storeFields, constants, propBindings) ??
      lowerPercentLengthStyleValue(field.expr, propertyName, storeFields, constants, propBindings) ??
      lowerRotateStyleDegrees(field.expr, propertyName, storeFields, constants, propBindings) ??
      lowerOpaqueColorStyleValue(field.expr, propertyName, storeFields, constants, propBindings) ??
      lowerKeywordStyleValue(field.expr, propertyName, storeFields, constants, propBindings) ??
      lowerTemplateExpression(field.expr, storeFields, constants, propBindings)
    if (!lowered || lowered.fields.some((storeField) => !storeField.readerName)) return false
  }
  return true
}

function emitTemplateStyleSlot(
  lines: string[],
  target: string,
  slot: GeaIrSlot,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
  fieldAccess: FieldAccess = BOXED_FIELD_ACCESS,
  disposerVar = 'disposer',
): boolean {
  const fields = styleObjectFieldsForSlot(slot, propBindings)
  if (!fields || fields.length === 0) return false
  const loweredFields: Array<{ lowered: LoweredExpression; bodyLines: string[] }> = []
  for (const field of fields) {
    const propertyName = cssPropertyName(field.name)
    const lowered =
      lowerPxLengthStyleValue(field.expr, propertyName, storeFields, constants, propBindings) ??
      lowerPercentLengthStyleValue(field.expr, propertyName, storeFields, constants, propBindings) ??
      lowerRotateStyleDegrees(field.expr, propertyName, storeFields, constants, propBindings) ??
      lowerOpaqueColorStyleValue(field.expr, propertyName, storeFields, constants, propBindings) ??
      lowerKeywordStyleValue(field.expr, propertyName, storeFields, constants, propBindings) ??
      lowerTemplateExpression(field.expr, storeFields, constants, propBindings)
    if (!lowered || lowered.fields.some((storeField) => !storeField.readerName)) return false
    const property = stylePropertyArgument(propertyName)
    const literalString = staticStringStyleValue(field.expr, constants, propBindings)
    const directStringLines = literalString !== null ? directStringStyleApplyLines(target, propertyName, literalString) : null
    const bodyLine = directStringLines ??
      (lowered.kind === 'number'
          ? styleNumberApplyLine(target, propertyName, loweredAsKind(lowered, 'number'), lowered.styleUnit) ??
            `gea::embedded::ui::StyleSheet::instance().applyNumberProperty(${target}, ${property}, static_cast<double>(${loweredAsKind(lowered, 'number')}));`
          : `gea::embedded::ui::StyleSheet::instance().applyProperty(${target}, ${property}, (${loweredAsString(lowered)}));`)
    loweredFields.push({ lowered, bodyLines: Array.isArray(bodyLine) ? bodyLine : [bodyLine] })
  }
  const staticFields = loweredFields.filter((item) => item.lowered.deps.length === 0 && item.lowered.fields.length === 0)
  const dynamicFields = loweredFields.filter((item) => item.lowered.deps.length > 0 || item.lowered.fields.length > 0)
  for (const item of staticFields) {
    for (const line of item.bodyLines) lines.push(`  ${line}`)
  }
  if (canCombineStyleLowerings(dynamicFields)) {
    const combined = mergeLowered('boolean', 'true', dynamicFields.map((item) => item.lowered))
    emitReactiveApply(lines, target, combined, dynamicFields.flatMap((item) => item.bodyLines), fieldAccess, [], disposerVar)
    return true
  }
  for (const item of dynamicFields) {
    emitReactiveApply(lines, target, item.lowered, item.bodyLines, fieldAccess, [], disposerVar)
  }
  return true
}

function canCombineStyleLowerings(items: Array<{ lowered: LoweredExpression }>): boolean {
  const fields = uniqueStoreFields(items.flatMap((item) => item.lowered.fields))
  const locals = new Set<string>()
  for (const field of fields) {
    const local = storeFieldLocalName(field.fieldName)
    if (locals.has(local)) return false
    locals.add(local)
  }
  return true
}

function styleObjectFieldsForSlot(slot: GeaIrSlot, propBindings: Map<string, PropBinding>): GeaIrExpressionObjectField[] | null {
  if (slot.exprObjectFields && slot.exprObjectFields.length > 0) return slot.exprObjectFields
  const prop = propNameForExpression(stripOuterParens(slot.expr?.trim() ?? ''))
  const binding = prop ? propBindings.get(prop) : null
  if (!binding) return null
  return parseExpressionObjectFields(resolveLiteralProps(binding.expr, binding.parentProps))
}

function parseExpressionObjectFields(expr: string): GeaIrExpressionObjectField[] | null {
  const text = stripOuterParens(expr.trim())
  if (!text.startsWith('{') || !text.endsWith('}')) return null
  const inner = text.slice(1, -1).trim()
  if (!inner) return []
  const fields: GeaIrExpressionObjectField[] = []
  for (const part of splitTopLevelOperator(inner, ',')) {
    const colon = findTopLevelOperator(part, ':')
    if (colon < 0) return null
    const name = classObjectKeyName(part.slice(0, colon).trim())
    if (!name) return null
    fields.push({ name, expr: part.slice(colon + 1).trim() })
  }
  return fields
}

function canEmitTemplateAttrSlot(
  slot: GeaIrSlot,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
): boolean {
  if (attributeSlotLines('root', slot)) return true
  const attrName = slotAttrName(slot)
  if (!attrName) return false
  const lowered = lowerTemplateExpression(slot.expr, storeFields, constants, propBindings)
  return !!lowered && !lowered.fields.some((field) => !field.readerName)
}

function emitTemplateAttrSlot(
  lines: string[],
  target: string,
  slot: GeaIrSlot,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
  fieldAccess: FieldAccess = BOXED_FIELD_ACCESS,
  disposerVar = 'disposer',
): boolean {
  const attrName = slotAttrName(slot)
  if (!attrName) return false
  const lowered = lowerTemplateExpression(slot.expr, storeFields, constants, propBindings)
  if (!lowered || lowered.fields.some((field) => !field.readerName)) return false
  emitReactiveApply(lines, target, lowered, `${target}.setAttribute(${cppString(attrName)}, (${loweredAsString(lowered)}).c_str());`, fieldAccess, [], disposerVar)
  return true
}

function lowerPxLengthStyleValue(
  expr: string | undefined,
  propertyName: string,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
): LoweredExpression | null {
  if (!expr || !isPxLengthStylePropertyName(propertyName)) return null
  const parts = parseTemplateLiteralParts(stripOuterParens(expr.trim()))
  if (!parts || parts.length !== 2) return null
  if (parts[0].kind !== 'expr' || parts[1].kind !== 'text' || parts[1].value !== 'px') return null
  const value = lowerTemplateExpression(parts[0].value, storeFields, constants, propBindings)
  if (!value) return null
  return { ...mergeLowered('number', loweredAsKind(value, 'number'), [value]), styleUnit: isBorderWidthStylePropertyName(propertyName) ? 'px' : undefined }
}

function lowerPercentLengthStyleValue(
  expr: string | undefined,
  propertyName: string,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
): LoweredExpression | null {
  if (!expr || !stylePercentPropertyEnumForPropertyName(propertyName)) return null
  const parts = parseTemplateLiteralParts(stripOuterParens(expr.trim()))
  if (!parts || parts.length !== 2) return null
  if (parts[0].kind !== 'expr' || parts[1].kind !== 'text' || parts[1].value !== '%') return null
  const value = lowerTemplateExpression(parts[0].value, storeFields, constants, propBindings)
  if (!value) return null
  return { ...mergeLowered('number', loweredAsKind(value, 'number'), [value]), styleUnit: 'percent' }
}

function lowerRotateStyleDegrees(
  expr: string | undefined,
  propertyName: string,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
): LoweredExpression | null {
  if (propertyName !== 'transform' && propertyName !== 'rotate') return null
  if (!expr) return null
  const parts = parseTemplateLiteralParts(stripOuterParens(expr.trim()))
  const degreesExpr = rotateDegreesTemplateExpression(parts, propertyName)
  if (!degreesExpr) return null
  const degrees = lowerTemplateExpression(degreesExpr, storeFields, constants, propBindings)
  if (!degrees) return null
  return mergeLowered('number', loweredAsKind(degrees, 'number'), [degrees])
}

function lowerOpaqueColorStyleValue(
  expr: string | undefined,
  propertyName: string,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
): LoweredExpression | null {
  if (!expr || !styleOpaqueColorTargetForPropertyName(propertyName)) return null
  const text = stripOuterParens(expr.trim())
  const literal = opaqueHexColorExpressionForStyleValue(text, constants, propBindings)
  if (literal) return { kind: 'number', expr: literal, deps: [], fields: [], styleUnit: 'color' }
  const ternary = splitTopLevelTernary(text)
  if (!ternary) return null
  const test = lowerTemplateExpression(ternary.test, storeFields, constants, propBindings)
  const consequent = opaqueHexColorExpressionForStyleValue(ternary.consequent, constants, propBindings)
  const alternate = opaqueHexColorExpressionForStyleValue(ternary.alternate, constants, propBindings)
  if (!test || !consequent || !alternate) return null
  return {
    ...mergeLowered('number', `(${loweredAsBoolean(test)} ? ${consequent} : ${alternate})`, [test]),
    styleUnit: 'color',
  }
}

function lowerKeywordStyleValue(
  expr: string | undefined,
  propertyName: string,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
): LoweredExpression | null {
  if (!expr || !styleKeywordPropertiesForPropertyName(propertyName)) return null
  const text = stripOuterParens(expr.trim())
  const literal = keywordCommonValue(keywordEntriesForExpr(propertyName, text, constants, propBindings))
  if (literal !== null) return { kind: 'number', expr: String(literal), deps: [], fields: [], styleUnit: 'keyword' }
  const ternary = splitTopLevelTernary(text)
  if (!ternary) return null
  const test = lowerTemplateExpression(ternary.test, storeFields, constants, propBindings)
  const consequent = keywordEntriesForExpr(propertyName, ternary.consequent, constants, propBindings)
  const alternate = keywordEntriesForExpr(propertyName, ternary.alternate, constants, propBindings)
  const consequentValue = keywordCommonValue(consequent)
  const alternateValue = keywordCommonValue(alternate)
  if (!test || consequentValue === null || alternateValue === null || !keywordEntriesCompatible(consequent, alternate)) return null
  return {
    ...mergeLowered('number', `(${loweredAsBoolean(test)} ? ${consequentValue} : ${alternateValue})`, [test]),
    styleUnit: 'keyword',
  }
}

function isRotateTransformPropertyName(propertyName: string): boolean {
  return propertyName === 'transform' || propertyName === 'rotate'
}

function rotateDegreesTemplateExpression(
  parts: Array<{ kind: 'text' | 'expr'; value: string }> | null,
  propertyName: string,
): string | null {
  if (!parts) return null
  if (propertyName === 'rotate') {
    if (parts.length !== 2) return null
    if (parts[0].kind !== 'expr' || parts[1].kind !== 'text' || parts[1].value.trim() !== 'deg') return null
    return parts[0].value
  }
  if (propertyName === 'transform') {
    if (parts.length !== 3) return null
    if (parts[0].kind !== 'text' || parts[1].kind !== 'expr' || parts[2].kind !== 'text') return null
    if (parts[0].value.trim() !== 'rotate(' || parts[2].value.trim() !== 'deg)') return null
    return parts[1].value
  }
  return null
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

function styleNumberApplyLine(node: string, propertyName: string, value: string, styleUnit?: 'percent' | 'color' | 'keyword' | 'px'): string | null {
  if (styleUnit === 'px') return `gea::embedded::ui::StyleSheet::instance().applyPixelLengthProperty(${node}, gea::embedded::ui::StyleDeclaration::${styleDeclarationEnumForPropertyName(propertyName)}, static_cast<double>(${value}));`
  if (styleUnit === 'percent') return directPercentStyleApplyLine(node, propertyName, value)
  if (styleUnit === 'color') return directColorStyleApplyLine(node, propertyName, value)
  if (styleUnit === 'keyword') return directKeywordStyleApplyLine(node, propertyName, value)
  return directNumberStyleApplyLine(node, propertyName, value)
}

function directPercentStyleApplyLine(node: string, propertyName: string, value: string): string | null {
  const property = stylePercentPropertyEnumForPropertyName(propertyName)
  return property
    ? `${node}.style().set(gea::embedded::ui::Property::${property}, static_cast<int>(std::round(static_cast<double>(${value}) * 10.0)));`
    : null
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

function keywordEntriesForExpr(
  propertyName: string,
  expr: string,
  constants: ConstantMap = new Map(),
  propBindings: Map<string, PropBinding> = new Map(),
): StylePropertyValue[] | null {
  const literal = staticStringStyleValue(expr, constants, propBindings)
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

function emitInputButtonValueText(
  lines: string[],
  varName: string,
  node: TemplateElement,
  nextVar: () => string
): void {
  if (node.tag.toLowerCase() !== 'input' || node.attrs?.type !== 'button') return
  const value = node.attrs.value
  if (!value) return
  const textVar = nextVar()
  lines.push(`  auto ${textVar} = gea::embedded::ui::Document::instance().createText();`)
  lines.push(`  ${textVar}.setText(${cppString(value)});`)
  lines.push(`  ${varName}.appendChild(${textVar});`)
}

function emitReactiveApply(
  lines: string[],
  target: string,
  lowered: LoweredExpression,
  bodyLine: string | string[],
  fieldAccess: FieldAccess = BOXED_FIELD_ACCESS,
  extraCaptures: string[] = [],
  disposerVar = 'disposer',
): void {
  const bodyLines = Array.isArray(bodyLine) ? bodyLine : [bodyLine]
  if (lowered.deps.length === 0 && lowered.fields.length === 0) {
    for (const line of bodyLines) lines.push(`  ${line}`)
    return
  }
  const captures = [target, ...extraCaptures]
  const fields = uniqueStoreFields(lowered.fields)
  // A field read against a DIFFERENT global store (e.g. `weather.temp` inside a
  // component) resolves through that store's global.
  const globalReceiver = singleGlobalReceiver(fields)
  if (globalReceiver) {
    const storeExpr = globalStoreExpression(globalReceiver)
    const deps = [...new Set(fields.flatMap(storeFieldReactiveDeps))].map((dep) => cppString(dep)).join(', ')
    const applyLines = [
      `std::function<void()>([${captures.join(', ')}]() mutable -> void {`,
      ...fields.map((field) => `    const auto ${storeFieldLocalName(field.fieldName)} = ${field.readerName}(${storeExpr});`),
      ...bodyLines.map((line) => `    ${line}`),
      '  })',
    ]
    // Compiled-base stores notify a typed SignalHub from every write path, so
    // static-dep binds register typed against the shared_ptr global directly —
    // no downcast, no boxed fallback. Lean/self stores keep the boxed
    // registration; gea_cpp_key boxes a typed cell at that dynamic boundary
    // (identity for already-boxed globals).
    const hubCapable =
      storeHubEmissionEnabled() && fields.every((field) => field.storeRuntimeBase === 'compiled' && !field.storeIsSelfStore)
    if (hubCapable) {
      lines.push(`  gea_rc_bind_hub_apply(${storeExpr}, ${disposerVar}, {${deps}}, ${applyLines[0]}`)
      for (const line of applyLines.slice(1, -1)) lines.push(`  ${line}`)
      lines.push(`  ${applyLines[applyLines.length - 1]});`)
      return
    }
    lines.push(`  bindReactiveApply(gea_cpp_key(${storeExpr}), ${disposerVar}, {${deps}}, [${captures.join(', ')}]() mutable -> void {`)
    for (const field of fields) {
      lines.push(`    const auto ${storeFieldLocalName(field.fieldName)} = ${field.readerName}(${storeExpr});`)
    }
    for (const line of bodyLines) lines.push(`    ${line}`)
    lines.push('  });')
    return
  }

  // Several DISTINCT global stores read by one renderer (e.g. `breakout.score` and
  // `fps.fpsText` in the same span). Read each field through its own store's global
  // accessor, and register the SAME apply once per store so a write to ANY of them
  // re-runs it. (singleGlobalReceiver returned null because >1 store is involved.)
  const globalGroups = groupGlobalStoreFields(fields)
  if (globalGroups) {
    const readLines = fields.map(
      (field) => `    const auto ${storeFieldLocalName(field.fieldName)} = ${field.readerName}(${fieldStoreArg(field)});`,
    )
    const hubCapable =
      storeHubEmissionEnabled() && fields.every((field) => field.storeRuntimeBase === 'compiled' && !field.storeIsSelfStore)
    for (const [receiver, groupFields] of globalGroups) {
      const storeExpr = globalStoreExpression(receiver)
      const deps = [...new Set(groupFields.flatMap(storeFieldReactiveDeps))].map((dep) => cppString(dep)).join(', ')
      if (hubCapable) {
        lines.push(`  gea_rc_bind_hub_apply(${storeExpr}, ${disposerVar}, {${deps}}, std::function<void()>([${captures.join(', ')}]() mutable -> void {`)
        for (const line of readLines) lines.push(`  ${line}`)
        for (const line of bodyLines) lines.push(`    ${line}`)
        lines.push('  }));')
      } else {
        lines.push(`  bindReactiveApply(gea_cpp_key(${storeExpr}), ${disposerVar}, {${deps}}, [${captures.join(', ')}]() mutable -> void {`)
        for (const line of readLines) lines.push(`  ${line}`)
        for (const line of bodyLines) lines.push(`    ${line}`)
        lines.push('  });')
      }
    }
    return
  }

  // The component's own store: boxed `bindReactiveApply(store, …)` or typed
  // per-field `self->dep.subscribe(…)`, chosen by the FieldAccess strategy.
  lines.push(...fieldAccess.bindReactiveOwn({ deps: lowered.deps, fields, captures, body: bodyLines, disposerVar }))
}

// Emit a synchronous "seed" of a reactive expression's CURRENT value, in a scoped
// block, declaring the same store-field locals the reactive binding uses. Emitted
// at node-creation time (before appendChild) so a parent whose only child is this
// value measures a non-zero size at first layout; without it, the reactive
// binding's (possibly deferred) initial apply runs after layout and a
// never-changing value (e.g. a store default) stays 0-sized and invisible.
function emitSyncSeed(lines: string[], lowered: LoweredExpression, bodyLines: string[]): void {
  if (lowered.deps.length === 0 && lowered.fields.length === 0) {
    for (const line of bodyLines) lines.push(`  ${line}`)
    return
  }
  const fields = uniqueStoreFields(lowered.fields)
  // Read each field through its OWN store's accessor (a renderer may read several
  // global stores), falling back to the boxed `store` param for own-store fields.
  lines.push('  {')
  for (const field of fields) lines.push(`    const auto ${storeFieldLocalName(field.fieldName)} = ${field.readerName}(${fieldStoreArg(field)});`)
  for (const line of bodyLines) lines.push(`    ${line}`)
  lines.push('  }')
}

// The receiver expression for reading one field: its global accessor
// (`__gea_global_<store>()`) for a global store, else the boxed `store` param.
function fieldStoreArg(field: StoreFieldRef): string {
  if (field.receiverName === 'this') return 'store'
  const isGlobal =
    !!field.receiverName &&
    (field.receiverName === field.storeGlobalName || /^[A-Z]/.test(field.receiverName) || !field.storeGlobalName)
  return isGlobal ? globalStoreExpression(field.receiverName!) : 'store'
}

// Group fields by their global store when a renderer reads MORE THAN ONE distinct
// global store (so each can be bound on its own typed hub). Returns null if any
// field isn't a global (own-store/`this`/local) or only one store is involved —
// those cases are handled by singleGlobalReceiver / the own-store path.
function groupGlobalStoreFields(fields: StoreFieldRef[]): Map<string, StoreFieldRef[]> | null {
  const groups = new Map<string, StoreFieldRef[]>()
  for (const field of fields) {
    if (field.receiverName === 'this') return null
    const isGlobal =
      !!field.receiverName &&
      (field.receiverName === field.storeGlobalName || /^[A-Z]/.test(field.receiverName) || !field.storeGlobalName)
    if (!isGlobal) return null
    const list = groups.get(field.receiverName!)
    if (list) list.push(field)
    else groups.set(field.receiverName!, [field])
  }
  return groups.size >= 2 ? groups : null
}

function singleGlobalReceiver(fields: StoreFieldRef[]): string | null {
  let receiver: string | null = null
  for (const field of fields) {
    // `this.field` is the component's OWN store (a self-store registers `this`
    // as its instance name purely for collision disambiguation) — never a
    // global; route through the own-store branch (boxed `store` / typed self).
    if (field.receiverName === 'this') return null
    // A receiver is a global store if it's the store's exported singleton name
    // (e.g. the lowercase `companion` in `export const companion = new ...`) or a
    // PascalCase store identifier. Without the storeGlobalName check, a lowercase
    // singleton falls back to `store` — the component's (often empty) store
    // context — so reactive reads in the template return empty.
    const isGlobal =
      !!field.receiverName &&
      (field.receiverName === field.storeGlobalName || /^[A-Z]/.test(field.receiverName) || !field.storeGlobalName)
    const next = isGlobal ? field.receiverName! : null
    if (!next) return null
    if (receiver && receiver !== next) return null
    receiver = next
  }
  return receiver
}

function canEmitIntrinsicImageMount(
  slot: GeaIrSlot,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
): boolean {
  if (mountSlotTag(slot) !== 'Image') return false
  const source = lowerImageSourceExpression(slot, storeFields, constants, propBindings)
  if (!source || source.fields.some((field) => !field.readerName)) return false
  const styleSlot = mountSlotStyleSlot(slot)
  return !styleSlot || canLowerTemplateStyleSlot(styleSlot, storeFields, constants, propBindings)
}

function emitIntrinsicImageMount(
  lines: string[],
  varName: string,
  slot: GeaIrSlot,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
  disposerVar = 'disposer',
): boolean {
  const source = lowerImageSourceExpression(slot, storeFields, constants, propBindings)
  if (!source || source.fields.some((field) => !field.readerName)) return false
  emitReactiveApply(
    lines,
    varName,
    source,
    `gea::embedded::ui::ImageElement(${varName}.id()).imageId(static_cast<int>(${loweredAsKind(source, 'number')}));`,
    BOXED_FIELD_ACCESS,
    [],
    disposerVar,
  )
  const styleSlot = mountSlotStyleSlot(slot)
  return !styleSlot || emitTemplateStyleSlot(lines, varName, styleSlot, storeFields, constants, propBindings, BOXED_FIELD_ACCESS, disposerVar)
}

function lowerImageSourceExpression(
  slot: GeaIrSlot,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
): LoweredExpression | null {
  for (const attr of mountSlotAttrs(slot)) {
    const parsed = parseMountAttr(attr)
    if (!parsed || parsed.name !== 'src') continue
    return lowerImageExpression(parsed.expr, storeFields, constants, propBindings)
  }
  return { kind: 'number', expr: '-1', deps: [], fields: [] }
}

function lowerImageExpression(
  expr: string,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
): LoweredExpression | null {
  const text = stripOuterParens(expr.trim())
  const ternary = splitTopLevelTernary(text)
  if (ternary) {
    const test = lowerTemplateExpression(ternary.test, storeFields, constants, propBindings)
    const consequent = lowerImageExpression(ternary.consequent, storeFields, constants, propBindings)
    const alternate = lowerImageExpression(ternary.alternate, storeFields, constants, propBindings)
    if (!test || !consequent || !alternate) return null
    return mergeLowered(
      'number',
      `(${loweredAsBoolean(test)} ? ${loweredAsKind(consequent, 'number')} : ${loweredAsKind(alternate, 'number')})`,
      [test, consequent, alternate],
    )
  }
  const global = simpleIdentifierValue(text)
  if (global) return { kind: 'number', expr: imageIdExpression(global), deps: [], fields: [] }
  const literalPath = stringLiteralValue(text)
  if (literalPath !== null) {
    return { kind: 'number', expr: `static_cast<double>(${imageSrcAssetIdExpression(literalPath)})`, deps: [], fields: [] }
  }
  return null
}

// Native `.id` field read off the `gea::host::GeaEmbeddedImage&` global —
// keep the shape in sync with imageIdApplyLine in cpp-mounted-lowering.ts.
function imageIdExpression(imageGlobal: string): string {
  return `static_cast<double>(__gea_global_${sanitizeCppIdentifier(imageGlobal)}().id)`
}

function mountPropBindings(slot: GeaIrSlot, parentProps: Map<string, PropBinding>): Map<string, PropBinding> {
  const out = new Map<string, PropBinding>()
  const attrs = mountSlotAttrs(slot)
  for (const attr of attrs) {
    const parsed = parseMountAttr(attr)
    if (parsed) out.set(parsed.name, { expr: parsed.expr, parentProps })
  }
  const childrenTemplate = mountSlotChildrenTemplate(slot)
  if (childrenTemplate && !out.has('children')) {
    out.set('children', { expr: 'props.children', parentProps, childrenTemplate })
  }
  return out
}

function mountSlotChildrenTemplate(slot: GeaIrSlot): GeaIrTemplate | null {
  const payload = slot.payload
  if (!payload || typeof payload !== 'object') return null
  const childrenTemplate = (payload as { childrenTemplate?: unknown }).childrenTemplate
  if (isGeaIrTemplate(childrenTemplate)) return childrenTemplate
  // The upstream IR only emits `childrenTemplate` when at least one child is
  // dynamic (an expression or another element). For pure static text — e.g.
  // `<Text class="settings-title">Settings</Text>` — the literal lives only in
  // `payload.children` as `JSXText` nodes and never reaches the bindings.
  // Without this recovery, every wrapper component with hard-coded text would
  // mount with an empty child node. Synthesize a no-slot template from the raw
  // JSX text so the static content survives inlining intact.
  const children = (payload as { children?: unknown }).children
  if (!Array.isArray(children) || children.length === 0) return null
  let text = ''
  for (const node of children) {
    if (!node || typeof node !== 'object') return null
    if ((node as { nodeType?: unknown }).nodeType !== 'JSXText') return null
    const code = (node as { code?: unknown }).code
    if (typeof code !== 'string') return null
    text += code
  }
  const collapsed = text.replace(/\s+/g, ' ').trim()
  if (!collapsed) return null
  return { html: collapsed, slots: [] }
}

function renderableTemplateText(text: string): string {
  const collapsed = text.replace(/\s+/g, ' ')
  return collapsed.trim() ? collapsed : ''
}

function isGeaIrTemplate(value: unknown): value is GeaIrTemplate {
  return !!value && typeof value === 'object' && typeof (value as { html?: unknown }).html === 'string' && Array.isArray((value as { slots?: unknown }).slots)
}

function childrenTemplateForPropSlot(slot: GeaIrSlot, propBindings: Map<string, PropBinding>): GeaIrTemplate | null {
  return childrenBindingForPropSlot(slot, propBindings)?.childrenTemplate ?? null
}

// A `props.children` placeholder inside a wrapped element (e.g. View's `{children}`
// position inside `<View class="settings-row">{Texts}</View>`) is expanded with
// the JSX subtree captured in the wrapper's mount slot. That subtree was *authored*
// in the caller's scope (SettingsRow's body, where `label`/`value` are bound), not
// in View's. Without using `binding.parentProps` when emitting it, references like
// `<Text>{label}</Text>` resolve against View's props — finding no `label`, and
// the text renders empty. Returning the full binding lets callers thread the
// caller-scope propBindings through.
function childrenBindingForPropSlot(slot: GeaIrSlot, propBindings: Map<string, PropBinding>): PropBinding | null {
  if (slot.kind !== 'text') return null
  const prop = propNameForExpression(stripOuterParens(slot.expr?.trim() ?? ''))
  if (!prop) return null
  const binding = propBindings.get(prop)
  return binding?.childrenTemplate ? binding : null
}

function conditionalBranchTemplates(slot: GeaIrSlot): { consequent: GeaIrTemplate | null; alternate: GeaIrTemplate | null } | null {
  const payload = slot.payload
  if (!payload || typeof payload !== 'object') return null
  const record = payload as { consequentTemplate?: unknown; alternateTemplate?: unknown }
  const consequent = isGeaIrTemplate(record.consequentTemplate) ? record.consequentTemplate : null
  const alternate = isGeaIrTemplate(record.alternateTemplate) ? record.alternateTemplate : null
  return { consequent, alternate }
}

function globalStoreExpression(receiver: string): string {
  return `__gea_global_${sanitizeCppIdentifier(receiver)}()`
}

function mountSlotAttrs(slot: GeaIrSlot): string[] {
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

function parseMountAttr(code: string): { name: string; expr: string } | null {
  const trimmed = code.trim()
  const stringAttr = trimmed.match(/^([A-Za-z_$][A-Za-z0-9_$]*)=(["'])(.*)\2$/s)
  if (stringAttr) return { name: stringAttr[1], expr: `${stringAttr[2]}${stringAttr[3]}${stringAttr[2]}` }
  const expressionAttr = trimmed.match(/^([A-Za-z_$][A-Za-z0-9_$]*)=\{([\s\S]*)\}$/)
  if (expressionAttr) return { name: expressionAttr[1], expr: expressionAttr[2].trim() }
  const bareAttr = trimmed.match(/^([A-Za-z_$][A-Za-z0-9_$]*)$/)
  return bareAttr ? { name: bareAttr[1], expr: 'true' } : null
}

function lowerTemplateExpression(
  expr: string | undefined,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
  seenProps = new Set<string>(),
): LoweredExpression | null {
  if (!expr) return null
  const text = stripOuterParens(expr.trim())
  if (text === 'true' || text === 'false') return { kind: 'boolean', expr: text, deps: [], fields: [] }
  const prop = propNameForExpression(text)
  if (prop) {
    const binding = propBindings.get(prop)
    if (binding) {
      if (seenProps.has(prop)) return null
      const nextSeen = new Set(seenProps)
      nextSeen.add(prop)
      return lowerTemplateExpression(binding.expr, storeFields, constants, binding.parentProps, nextSeen)
    }
    const constant = constantExpression(prop, constants)
    if (constant) return { kind: kindForShape(constant.shape), expr: constant.expr, deps: [], fields: [] }
    if (isRuntimeNumericConstantIdentifier(prop)) {
      return { kind: 'number', expr: `static_cast<double>(${prop})`, deps: [], fields: [] }
    }
  }

  const ternary = splitTopLevelTernary(text)
  if (ternary) {
    const test = lowerTemplateExpression(ternary.test, storeFields, constants, propBindings, seenProps)
    const consequent = lowerTemplateExpression(ternary.consequent, storeFields, constants, propBindings, seenProps)
    const alternate = lowerTemplateExpression(ternary.alternate, storeFields, constants, propBindings, seenProps)
    if (!test || !consequent || !alternate) return null
    return mergeLowered(
      consequent.kind === 'string' || alternate.kind === 'string' ? 'string' : consequent.kind,
      `(${loweredAsBoolean(test)} ? ${loweredAsKind(consequent, consequent.kind === 'string' || alternate.kind === 'string' ? 'string' : consequent.kind)} : ${loweredAsKind(alternate, consequent.kind === 'string' || alternate.kind === 'string' ? 'string' : consequent.kind)})`,
      [test, consequent, alternate],
    )
  }

  const templateLiteral = lowerTemplateLiteral(text, storeFields, constants, propBindings, seenProps)
  if (templateLiteral) return templateLiteral

  const logicalOr = splitTopLevelOperator(text, '||')
  if (logicalOr.length > 1) {
    const lowered = logicalOr.map((part) => lowerTemplateExpression(part, storeFields, constants, propBindings, seenProps))
    if (lowered.some((part) => !part)) return null
    return mergeLowered('boolean', lowered.map((part) => loweredAsBoolean(part!)).join(' || '), lowered as LoweredExpression[])
  }

  const logicalAnd = splitTopLevelOperator(text, '&&')
  if (logicalAnd.length > 1) {
    const lowered = logicalAnd.map((part) => lowerTemplateExpression(part, storeFields, constants, propBindings, seenProps))
    if (lowered.some((part) => !part)) return null
    return mergeLowered('boolean', lowered.map((part) => loweredAsBoolean(part!)).join(' && '), lowered as LoweredExpression[])
  }

  const plusParts = splitTopLevelOperator(text, '+')
  if (plusParts.length > 1) {
    const lowered = plusParts.map((part) => lowerTemplateExpression(part, storeFields, constants, propBindings, seenProps))
    if (lowered.some((part) => !part)) return null
    if (lowered.every((part) => part!.kind === 'number')) {
      return mergeLowered('number', lowered.map((part) => part!.expr).join(' + '), lowered as LoweredExpression[])
    }
    return mergeLowered('string', lowered.map((part) => loweredAsString(part!)).join(' + '), lowered as LoweredExpression[])
  }

  if (text.startsWith('-') && numericLiteralValue(text) === null) {
    const operand = lowerTemplateExpression(text.slice(1), storeFields, constants, propBindings, seenProps)
    if (!operand) return null
    return mergeLowered('number', `(-${loweredAsNumberOperand(operand)})`, [operand])
  }

  const unaryMathMatch = text.match(/^Math\.(abs|floor|ceil|round)\(([\s\S]*)\)$/)
  if (unaryMathMatch) {
    const operand = lowerTemplateExpression(unaryMathMatch[2], storeFields, constants, propBindings, seenProps)
    if (!operand) return null
    const fn =
      unaryMathMatch[1] === 'abs'
        ? 'std::abs'
        : unaryMathMatch[1] === 'floor'
          ? 'std::floor'
          : unaryMathMatch[1] === 'ceil'
            ? 'std::ceil'
            : 'std::round'
    return mergeLowered('number', `${fn}(${loweredAsNumberOperand(operand)})`, [operand])
  }

  const arithmetic = splitTopLevelArithmetic(text)
  if (arithmetic) {
    const left = lowerTemplateExpression(arithmetic.left, storeFields, constants, propBindings, seenProps)
    const right = lowerTemplateExpression(arithmetic.right, storeFields, constants, propBindings, seenProps)
    if (!left || !right) return null
    return mergeLowered('number', `(${loweredAsNumberOperand(left)} ${arithmetic.op} ${loweredAsNumberOperand(right)})`, [left, right])
  }

  const binary = splitTopLevelBinary(text)
  if (binary) {
    const left = lowerTemplateExpression(binary.left, storeFields, constants, propBindings, seenProps)
    const right = lowerTemplateExpression(binary.right, storeFields, constants, propBindings, seenProps)
    if (!left || !right) return null
    const equality = loweredEqualityExpression(left, right, binary.op)
    if (equality) return mergeLowered('boolean', equality, [left, right])
    return mergeLowered('boolean', `(${loweredAsComparable(left)} ${cppBinaryOperator(binary.op)} ${loweredAsComparable(right)})`, [left, right])
  }

  if (text.startsWith('!')) {
    const operand = lowerTemplateExpression(text.slice(1), storeFields, constants, propBindings, seenProps)
    if (!operand) return null
    return mergeLowered('boolean', `!(${loweredAsBoolean(operand)})`, [operand])
  }

  const string = stringLiteralValue(text)
  if (string !== null) return { kind: 'string', expr: `std::string(${cppString(string)})`, deps: [], fields: [] }
  if (numericLiteralValue(text) !== null) return { kind: 'number', expr: `static_cast<double>(${text})`, deps: [], fields: [] }

  const memberLength = text.match(/^([A-Za-z_$][A-Za-z0-9_$]*)\.([A-Za-z_$][A-Za-z0-9_$]*)\.length$/)
  if (memberLength) {
    const field = storeFieldByName(memberLength[2], storeFields, memberLength[1])
    // `.length` lowers to native `.size()` for arrays AND strings — a string
    // store field is a `std::string` member, so `{store.toast.length}` reads
    // the same scalar without boxing (JS string .length === size() for the
    // template-expression subset, which never emits non-ASCII field text).
    const lengthCarrier = field?.shape?.kind === 'array' || (field?.shape?.kind === 'literal' && field.shape.valueType === 'string')
    if (!field || !lengthCarrier) return null
    if (!field.readerName) {
      // Named arrays whose element records contain nested data may not have a
      // value reader, but their concrete store member still has a native
      // `.size()`. Read only that scalar so `{store.books.length}` stays typed
      // without materializing or boxing the array.
      if (field.storeGlobalName !== sanitizeCppIdentifier(memberLength[1]) || field.storeIsSelfStore) return null
      const fieldRef: StoreFieldRef = {
        ...field,
        fieldType: 'double',
        readerName: `[](const auto &__gea_store) -> double { return __gea_store ? static_cast<double>(__gea_store->${field.fieldName}.size()) : 0.0; }`,
        shape: { kind: 'literal', valueType: 'number' },
        receiverName: memberLength[1],
      }
      return {
        kind: 'number',
        expr: storeFieldLocalName(field.fieldName),
        deps: [field.fieldName],
        fields: [fieldRef],
      }
    }
    const fieldRef: StoreFieldRef = { ...field, receiverName: memberLength[1] }
    return {
      kind: 'number',
      expr: `static_cast<double>(${storeFieldLocalName(field.fieldName)}.size())`,
      deps: [field.fieldName],
      fields: [fieldRef],
    }
  }

  const member = text.match(/^([A-Za-z_$][A-Za-z0-9_$]*)\.([A-Za-z_$][A-Za-z0-9_$]*)$/)
  if (member) {
    const field = storeFieldByName(member[2], storeFields, member[1])
    if (field?.readerName) {
      const fieldRef: StoreFieldRef = { ...field, receiverName: member[1] }
      return {
        kind: kindForStoreField(field),
        expr: storeFieldLocalName(field.fieldName),
        deps: storeFieldReactiveDeps(field),
        fields: [fieldRef],
      }
    }
    // A member read against a global store singleton that the per-component IR
    // doesn't enumerate (e.g. `voiceNotesLibrary.noteListBackSelected` when the
    // store lives in another module) still has a runtime home: the global store
    // hub. Lower it to a dynamic boxed read through `__gea_global_<receiver>()`
    // — a reactive binding keyed on the field name — instead of failing the
    // whole renderer back to the slow runtime-template path.
    const dynamicField = dynamicGlobalStoreFieldRef(member[1], member[2], constants, propBindings)
    if (dynamicField) {
      return {
        kind: 'value',
        expr: storeFieldLocalName(dynamicField.fieldName),
        deps: [dynamicField.fieldName],
        fields: [dynamicField],
      }
    }
    return null
  }

  return null
}

// Synthesize a reactive field reference for a member read whose receiver is a
// global store singleton not modeled in `storeFields`. The read lowers through
// the global store hub via an inline-lambda reader so it slots into every
// `${field.readerName}(${storeExpr})` site unchanged: the lambda boxes the hub
// accessor (typed `shared_ptr<Store>` or `gea_cpp_value`) and reads the field
// dynamically. Returns null for host facades / JS globals / locally-bound names.
function dynamicGlobalStoreFieldRef(
  receiver: string,
  property: string,
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
): StoreFieldRef | null {
  if (NON_STORE_RECEIVERS.has(receiver)) return null
  if (constants.has(receiver) || propBindings.has(receiver)) return null
  const reader = `[](auto&& __gea_store) { return gea_cpp_key(__gea_store).record_get_literal(${cppString(property)}); }`
  return {
    storeClass: '',
    stateType: '',
    fieldName: property,
    fieldType: 'gea_cpp_value',
    readerName: reader,
    field: { name: property },
    shape: null,
    reader: null,
    storeGlobalName: receiver,
    receiverName: receiver,
  }
}

function lowerTemplateLiteral(
  expr: string,
  storeFields: StoreFieldPlan[],
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
  seenProps: Set<string>,
): LoweredExpression | null {
  if (!expr.startsWith('`') || !expr.endsWith('`')) return null
  const parts = parseTemplateLiteralParts(expr)
  if (!parts) return null
  const loweredParts: LoweredExpression[] = []
  const exprParts: string[] = []
  for (const part of parts) {
    if (part.kind === 'text') {
      if (part.value.length === 0) continue
      exprParts.push(`std::string(${cppString(part.value)})`)
      continue
    }
    const lowered = lowerTemplateExpression(part.value, storeFields, constants, propBindings, seenProps)
    if (!lowered) return null
    loweredParts.push(lowered)
    exprParts.push(loweredAsString(lowered))
  }
  return mergeLowered('string', exprParts.length > 0 ? exprParts.join(' + ') : 'std::string("")', loweredParts)
}

function parseTemplateLiteralParts(expr: string): Array<{ kind: 'text' | 'expr'; value: string }> | null {
  const body = expr.slice(1, -1)
  const parts: Array<{ kind: 'text' | 'expr'; value: string }> = []
  let text = ''
  for (let index = 0; index < body.length; index += 1) {
    const char = body[index]
    if (char === '\\') {
      const next = body[index + 1]
      if (next === undefined) return null
      text += next === 'n' ? '\n' : next === 't' ? '\t' : next
      index += 1
      continue
    }
    if (char !== '$' || body[index + 1] !== '{') {
      text += char
      continue
    }
    if (text) {
      parts.push({ kind: 'text', value: text })
      text = ''
    }
    const end = findTemplateExpressionEnd(body, index + 2)
    if (end < 0) return null
    parts.push({ kind: 'expr', value: body.slice(index + 2, end).trim() })
    index = end
  }
  if (text) parts.push({ kind: 'text', value: text })
  return parts
}

function findTemplateExpressionEnd(expr: string, start: number): number {
  let depth = 1
  let quote: string | null = null
  let escaped = false
  for (let index = start; index < expr.length; index += 1) {
    const char = expr[index]
    if (quote) {
      if (escaped) escaped = false
      else if (char === '\\') escaped = true
      else if (char === quote) quote = null
      continue
    }
    if (char === '"' || char === "'" || char === '`') {
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

function propNameForExpression(expr: string): string | null {
  const direct = expr.match(/^[A-Za-z_$][A-Za-z0-9_$]*$/)
  if (direct) return direct[0]
  // Match both `this.props.X` (class-component prop access, emitted by the
  // class-component transform) and `props.X` (functional-component prop
  // access, emitted by `rewriteFnComponent`). Without the `props.X` branch,
  // lowering fails for every wrapper-style functional component (View, Text,
  // Button, …) because their slot expressions read `props.class`, etc., and
  // we need to substitute those into the parent's bound prop value.
  const propsAccess = expr.match(/^(?:this\.)?props\.([A-Za-z_$][A-Za-z0-9_$]*)$/)
  return propsAccess ? propsAccess[1] : null
}

function simpleIdentifierValue(expr: string): string | null {
  const text = expr.trim()
  return /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(text) ? text : null
}

function cssPropertyName(name: string): string {
  return name.replace(/[A-Z]/g, (letter) => `-${letter.toLowerCase()}`)
}

function storeFieldByName(name: string, storeFields: StoreFieldPlan[], receiver?: string): StoreFieldPlan | null {
  const sanitized = sanitizeCppIdentifier(name)
  const matches = storeFields.filter((field) => field.fieldName === sanitized)
  if (matches.length === 1) return matches[0]
  // The field name collides across several bundled stores (e.g. an app store
  // and the built-in SettingsStore both declare `status`/`screen`). Use the
  // receiver to pick the store whose exported instance matches — `store.status`
  // resolves against the store exported as `store`. Without this the slot fails
  // to lower and the component falls back to a path that renders the raw `0`
  // text placeholder instead of the bound value.
  if (matches.length > 1 && receiver) {
    const recv = sanitizeCppIdentifier(receiver)
    const byReceiver = matches.filter((field) => field.storeGlobalName === recv)
    if (byReceiver.length === 1) return byReceiver[0]
  }
  return null
}

function kindForShape(shape: GeaIrStoreValueShape | null | undefined): LoweredExpression['kind'] {
  if (shape?.kind !== 'literal') return 'value'
  if (shape.valueType === 'string' || shape.valueType === 'number' || shape.valueType === 'boolean') return shape.valueType
  return 'value'
}

function kindForStoreField(field: StoreFieldPlan): LoweredExpression['kind'] {
  const shaped = kindForShape(field.shape)
  if (shaped !== 'value') return shaped
  const fieldType = field.fieldType.trim()
  if (fieldType === 'double' || fieldType === 'gea_number') return 'number'
  if (fieldType === 'std::string') return 'string'
  if (fieldType === 'bool') return 'boolean'
  return 'value'
}

function storeFieldReactiveDeps(field: StoreFieldPlan): string[] {
  return field.isGetter ? (field.reactiveDeps ?? []) : [field.fieldName]
}

function mergeLowered(kind: LoweredExpression['kind'], expr: string, parts: LoweredExpression[]): LoweredExpression {
  return {
    kind,
    expr,
    deps: parts.flatMap((part) => part.deps),
    fields: uniqueStoreFields(parts.flatMap((part) => part.fields)),
  }
}

function loweredAsKind(lowered: LoweredExpression, kind: LoweredExpression['kind']): string {
  if (kind === 'string') return loweredAsString(lowered)
  if (kind === 'boolean') return loweredAsBoolean(lowered)
  if (kind === 'number') return lowered.kind === 'number' ? lowered.expr : `gea::runtime::coerce::to_number(${lowered.expr})`
  return lowered.expr
}

function loweredAsNumberOperand(lowered: LoweredExpression): string {
  return `(${loweredAsKind(lowered, 'number')})`
}

function loweredAsString(lowered: LoweredExpression): string {
  return lowered.kind === 'string' ? lowered.expr : `gea_cpp_to_string(${lowered.expr})`
}

function loweredAsBoolean(lowered: LoweredExpression): string {
  if (lowered.kind === 'boolean') return lowered.expr
  return `gea::runtime::coerce::to_boolean(${lowered.expr})`
}

function loweredAsComparable(lowered: LoweredExpression): string {
  if (lowered.kind === 'string') return lowered.expr
  if (lowered.kind === 'number' || lowered.kind === 'boolean') return lowered.expr
  return `gea_cpp_key(${lowered.expr})`
}

function loweredEqualityExpression(left: LoweredExpression, right: LoweredExpression, op: string): string | null {
  // When both operands are the SAME comparable primitive (string/number/boolean),
  // emit a NATIVE C++ comparison — never re-box a typed std::string/double/bool
  // through gea_cpp_key just to run `==`. For matching static types JS `==` and
  // `===` are equivalent (no coercion happens), so both collapse to native `==`,
  // and `!=`/`!==` to native `!=`. Only genuinely dynamic or mixed-type operands
  // fall through to the boxed gea_cpp_value path below. Mirrors loweredAsComparable.
  const nativeComparable = left.kind === right.kind && (left.kind === 'string' || left.kind === 'number' || left.kind === 'boolean')
  if (nativeComparable) {
    if (op === '===' || op === '==') return `(${left.expr} == ${right.expr})`
    if (op === '!==' || op === '!=') return `(${left.expr} != ${right.expr})`
  }
  if (op === '===') return `gea_cpp_strict_equals(gea_cpp_key(${left.expr}), gea_cpp_key(${right.expr}))`
  if (op === '==') return `gea_cpp_loose_equals(gea_cpp_key(${left.expr}), gea_cpp_key(${right.expr}))`
  if (op === '!==') return `(!gea_cpp_strict_equals(gea_cpp_key(${left.expr}), gea_cpp_key(${right.expr})))`
  if (op === '!=') return `(!gea_cpp_loose_equals(gea_cpp_key(${left.expr}), gea_cpp_key(${right.expr})))`
  return null
}

function cppBinaryOperator(op: string): string {
  if (op === '===' || op === '==') return '=='
  if (op === '!==' || op === '!=') return '!='
  return op
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

function splitTopLevelTernary(expr: string): { test: string; consequent: string; alternate: string } | null {
  let quote: string | null = null
  let escaped = false
  let depth = 0
  let question = -1
  let ternaryDepth = 0
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
    else if (char === '?' && depth === 0) {
      if (question < 0) question = index
      else ternaryDepth += 1
    } else if (char === ':' && depth === 0 && question >= 0) {
      if (ternaryDepth > 0) {
        ternaryDepth -= 1
        continue
      }
      return {
        test: expr.slice(0, question).trim(),
        consequent: expr.slice(question + 1, index).trim(),
        alternate: expr.slice(index + 1).trim(),
      }
    }
  }
  return null
}

function splitTopLevelBinary(expr: string): { left: string; op: string; right: string } | null {
  for (const op of ['===', '!==', '>=', '<=', '==', '!=', '>', '<']) {
    const index = findTopLevelOperator(expr, op)
    if (index < 0) continue
    return {
      left: expr.slice(0, index).trim(),
      op,
      right: expr.slice(index + op.length).trim(),
    }
  }
  return null
}

function splitTopLevelArithmetic(expr: string): { left: string; op: string; right: string } | null {
  return splitLastTopLevelOperator(expr, ['-']) ?? splitLastTopLevelOperator(expr, ['*', '/'])
}

function splitLastTopLevelOperator(expr: string, operators: string[]): { left: string; op: string; right: string } | null {
  const match = findLastTopLevelOperator(expr, operators)
  if (!match || match.index <= 0) return null
  const left = expr.slice(0, match.index).trim()
  const right = expr.slice(match.index + match.op.length).trim()
  return left && right ? { left, op: match.op, right } : null
}

function findLastTopLevelOperator(expr: string, operators: string[]): { index: number; op: string } | null {
  let quote: string | null = null
  let escaped = false
  let depth = 0
  let last: { index: number; op: string } | null = null
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
    if (depth !== 0) continue
    for (const op of operators) {
      if (index === 0 || !expr.startsWith(op, index)) continue
      last = { index, op }
    }
  }
  return last
}

function splitTopLevelOperator(expr: string, operator: string): string[] {
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
    else if (depth === 0 && expr.startsWith(operator, index)) {
      parts.push(expr.slice(start, index).trim())
      start = index + operator.length
      index += operator.length - 1
    }
  }
  parts.push(expr.slice(start).trim())
  return parts.filter(Boolean)
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

function numericLiteralValue(expr: string): string | null {
  const text = expr.trim()
  return /^-?(?:\d+|\d+\.\d+|\.\d+)$/.test(text) ? text : null
}

function opaqueHexColorExpressionForStyleValue(
  expr: string,
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
): string | null {
  const literal = staticStringStyleValue(expr, constants, propBindings)
  const color = literal === null ? null : opaqueCssHexColor(literal)
  return color ? nativeStyleColorExpression(color) : null
}

function staticStringStyleValue(
  expr: string | undefined,
  constants: ConstantMap,
  propBindings: Map<string, PropBinding>,
  seenProps = new Set<string>(),
): string | null {
  if (!expr) return null
  const text = stripOuterParens(expr.trim())
  const literal = stringLiteralValue(text)
  if (literal !== null) return literal
  const prop = propNameForExpression(text)
  if (!prop) return null
  const binding = propBindings.get(prop)
  if (binding) {
    if (seenProps.has(prop)) return null
    const nextSeen = new Set(seenProps)
    nextSeen.add(prop)
    return staticStringStyleValue(binding.expr, constants, binding.parentProps, nextSeen)
  }
  const constant = constants.get(prop)
  return constant?.valueType === 'string' ? constant.value : null
}

function stringLiteralValue(expr: string): string | null {
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

function uniqueStoreFields(fields: StoreFieldRef[]): StoreFieldRef[] {
  const seen = new Set<string>()
  const out: StoreFieldRef[] = []
  for (const field of fields) {
    const key = `${field.receiverName ?? ''}.${field.storeClass}.${field.fieldName}`
    if (seen.has(key)) continue
    seen.add(key)
    out.push(field)
  }
  return out
}

function uniqueStrings(values: string[]): string[] {
  return [...new Set(values)]
}

export function parseTemplateRoot(html: string): TemplateElement | null {
  const children = parseTemplateFragmentChildren(html)
  return children?.find((child): child is TemplateElement => child.kind === 'element') ?? null
}

function parseTemplateFragmentChildren(html: string): TemplateChild[] | null {
  const fragment: TemplateElement = { kind: 'element', tag: '#fragment', children: [] }
  const stack: TemplateElement[] = [fragment]
  let index = 0
  while (index < html.length) {
    if (html.startsWith('<!--', index)) {
      const end = html.indexOf('-->', index + 4)
      if (end < 0) return null
      const value = Number.parseInt(html.slice(index + 4, end).trim(), 10)
      if (Number.isFinite(value)) stack[stack.length - 1].children.push({ kind: 'slot', index: value })
      index = end + 3
      continue
    }
    if (html.startsWith('</', index)) {
      const end = html.indexOf('>', index + 2)
      if (end < 0) break
      if (stack.length > 1) stack.pop()
      index = end + 1
      continue
    }
    if (html[index] === '<') {
      const end = html.indexOf('>', index + 1)
      if (end < 0) break
      const raw = html.slice(index + 1, end).trim()
      const selfClosing = raw.endsWith('/')
      const tag = raw.match(/^([A-Za-z][A-Za-z0-9:-]*)/)?.[1]
      if (!tag) return null
      const attrs = parseStaticAttrs(raw)
      const child: TemplateElement = { kind: 'element', tag, children: [], ...(attrs.class ? { className: attrs.class } : {}), attrs }
      stack[stack.length - 1].children.push(child)
      if (!selfClosing && !isVoidTag(tag)) stack.push(child)
      index = end + 1
      continue
    }
    const next = html.indexOf('<', index)
    const end = next < 0 ? html.length : next
    const text = html.slice(index, end)
    if (text.trim()) stack[stack.length - 1].children.push({ kind: 'text', text })
    index = end
  }
  return fragment.children
}

function pathKey(path: number[]): string {
  return path.join('.')
}

function escapeRegExp(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
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

function isTextNodeTag(tag: string): boolean {
  const normalized = tag.toLowerCase()
  return normalized === 'span' || normalized === 'p' || /^h[1-6]$/.test(normalized)
}

function canUseTextNodeForElement(node: TemplateElement): boolean {
  if (!isTextNodeTag(node.tag)) return false
  if (node.children.some((child) => child.kind === 'element')) return false
  // A Text node holds exactly ONE run of text. Collapse the element into a text
  // node only when it has a single text-producing child (one static text, or one
  // reactive placeholder). An element with multiple runs — e.g.
  // `<span>{a} / {b}</span>` (children: slot, " / ", slot) — must stay a VIEW
  // container so each run becomes its own child text node laid out side by side.
  // Baking one run into the parent via `setText` while appending the others as
  // children stacks them all at the parent's origin (the device-only overlap
  // where "0 / 2" renders as one garbled glyph).
  let runs = 0
  for (const child of node.children) {
    if (child.kind === 'text') {
      if (renderableTemplateText(child.text)) runs += 1
    } else {
      // A slot placeholder (reactive text / inlined children) is a text run.
      runs += 1
    }
    if (runs > 1) return false
  }
  return true
}

function intrinsicStyleLines(varName: string, node: TemplateElement): string[] {
  const normalized = node.tag.toLowerCase()
  const lines: string[] = []
  // A paragraph carrying inline children (<p>copy <em>x</em> more</p>) used to be
  // lowered as `display: flex; flex-direction: row; flex-wrap: wrap`, because the
  // engine had no inline formatting and a plain block would have stacked the runs.
  // It does now: a block whose in-flow children are all inline-level synthesizes an
  // inline formatting context (LayoutNodePass::resolveInlineFormattingRow), which
  // flows real line boxes — baseline-aligned, always wrapping, never flex-shrunk,
  // and splitting a text run ACROSS lines so the box after it resumes on the run's
  // last line. Forcing display:flex here defeated all of that: setDefaultStyle goes
  // through the class-rule path, which marks `display` explicit, so the engine could
  // not tell the lowering apart from an authored `display: flex` and every run stayed
  // an atomic flex item.
  const headingSize = headingDefaultFontSize(normalized)
  if (headingSize !== null) lines.push(defaultStyleLine(varName, 'FontSize', headingSize))
  return lines
}

function headingDefaultFontSize(tag: string): number | null {
  if (tag === 'h1') return 34
  if (tag === 'h2') return 28
  if (tag === 'h3') return 24
  if (tag === 'h4') return 20
  if (tag === 'h5') return 18
  if (tag === 'h6') return 16
  return null
}

function defaultStyleLine(varName: string, property: string, value: number): string {
  return `  gea::embedded::ui::Tree::instance().setDefaultStyle(${varName}.id(), gea::embedded::ui::Property::${property}, ${value});`
}

function isVoidTag(tag: string): boolean {
  return ['area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'param', 'source', 'track', 'wbr'].includes(tag.toLowerCase())
}

function uiCreateNodeExpression(node: TemplateElement): string | null {
  const normalized = node.tag.toLowerCase()
  if (normalized === 'canvas') return 'gea::embedded::ui::Document::instance().createCanvas()'
  if (normalized === 'camera') return 'gea::embedded::ui::Document::instance().createCamera()'
  if (normalized === 'audio') return 'gea::embedded::ui::Document::instance().createAudio()'
  if (normalized === 'img') return 'gea::embedded::ui::Document::instance().createImage()'
  if (normalized === 'virtual-list') return 'gea::embedded::ui::Document::instance().createVirtualList()'
  if (normalized === 'input' && node.attrs?.type === 'button') return 'gea::embedded::ui::Document::instance().createButton()'
  if (normalized === 'button') return 'gea::embedded::ui::Document::instance().createButton()'
  // The IR wraps fragment / component-rooted templates in
  // `<span style="display:contents">…</span>`. That wrapper is a container,
  // not a text run — treat it as a view so its mount/keyed-list/conditional
  // children get proper child nodes appended. Without this, the wrapper
  // collapses to a text node and `canMountComponent` fails for any
  // functional component whose root JSX is itself a component (SettingsPanel,
  // SettingsContent, all the wrapper-style library components).
  if (isTransparentFragmentWrapper(node)) return 'gea::embedded::ui::Document::instance().createView()'
  if (canUseTextNodeForElement(node)) return 'gea::embedded::ui::Document::instance().createText()'
  return 'gea::embedded::ui::Document::instance().createView()'
}

function isTransparentFragmentWrapper(node: TemplateElement): boolean {
  if (node.tag.toLowerCase() !== 'span') return false
  const attrs = node.attrs ?? {}
  const keys = Object.keys(attrs)
  if (keys.length !== 1 || keys[0] !== 'style') return false
  return /\bdisplay\s*:\s*contents\b/i.test(attrs.style)
}

// True when the slot's expression is a bare `props.X` reference and the
// parent didn't pass `X`. Used so wrapper components like `<View>` — which
// expose lots of optional props (`style`, `pressId`, `onClick`, `children`,
// etc.) — can be inlined as direct-mount children even when the parent only
// binds a subset. Conservative: anything beyond a bare `props.X` (composite
// expressions, ternaries that read multiple props, etc.) is left to the
// normal lowering pipeline, which still emits the right code when the props
// are bound and still fails loudly when they're not.
function slotReferencesUnboundProp(slot: GeaIrSlot, propBindings: Map<string, PropBinding>): boolean {
  if (slot.exprPath && slot.exprPath.length === 2 && slot.exprPath[0] === 'props') {
    return !propBindings.has(slot.exprPath[1])
  }
  if (slot.expr) {
    const match = slot.expr.match(/^props\.([A-Za-z_$][A-Za-z0-9_$]*)$/)
    if (match) return !propBindings.has(match[1])
  }
  return false
}
