import {
  arrayElementAliasFieldTarget,
  arrayItemAliasKey,
  arrayItemFieldAliasKey,
  arrayItemFieldHint,
  fieldHint,
  localArrayCppType,
  localArrayTypeRef,
  nextStoreMethodTemp,
  typedArrayItemFieldAccess,
  type StoreMethodHint,
  type StoreMethodLowerContext
} from './cpp-store-method-context.js'
import {
  exprPath,
  thisArrayItemFieldTarget,
  thisArrayLengthTarget,
  thisArrayPopTarget,
  thisArrayPushTarget,
  thisArrayShiftTarget,
  thisArraySpliceTarget,
  thisArrayUnshiftTarget,
  thisFieldName,
} from './cpp-store-method-targets.js'
import type { GeaIrStoreExpr } from './types.js'
import { sanitizeCppIdentifier } from './utils.js'

export function lowerExpr(context: StoreMethodLowerContext, expr: GeaIrStoreExpr, hint: StoreMethodHint): string | null {
  if (expr.kind === 'number') return constantExpr(expr.value, hint)
  if (expr.kind === 'string') return hint === 'string' || hint === 'any' ? `std::string(${JSON.stringify(expr.value)})` : JSON.stringify(expr.value)
  if (expr.kind === 'boolean') return constantExpr(expr.value, hint)
  if (expr.kind === 'null') return constantExpr(null, hint)
  if (expr.kind === 'identifier') return lowerIdentifier(context, expr.name, hint)
  if (expr.kind === 'this') return '(*this)'
  if (expr.kind === 'member') return lowerMember(context, expr, hint)
  if (expr.kind === 'index') return lowerIndex(context, expr, hint)
  if (expr.kind === 'call') return lowerCall(context, expr, hint)
  if (expr.kind === 'object') return lowerObject(context, expr)
  if (expr.kind === 'array') return lowerArray(context, expr)
  if (expr.kind === 'unary') {
    const arg = lowerExpr(context, expr.arg, expr.op === '!' ? 'boolean' : 'number')
    return arg ? `${expr.op}${parenthesize(arg)}` : null
  }
  if (expr.kind === 'binary' || expr.kind === 'logical') return lowerBinary(context, expr, hint)
  if (expr.kind === 'conditional') return lowerConditional(context, expr, hint)
  if (expr.kind === 'update') return lowerForUpdate(context, expr)
  return null
}

export function lowerForUpdate(context: StoreMethodLowerContext, expr: GeaIrStoreExpr): string | null {
  if (expr.kind === 'update' && expr.arg.kind === 'identifier') return `${expr.op === '++' ? '++' : '--'}${sanitizeCppIdentifier(expr.arg.name)}`
  return lowerExpr(context, expr, 'any')
}

export function assignmentHint(target: GeaIrStoreExpr): StoreMethodHint {
  const path = exprPath(target)
  if (path?.endsWith('.frequency.value')) return 'number'
  if (path?.endsWith('.type')) return 'string'
  return 'any'
}

// The JS URI globals lower to the gea::uri runtime helpers (same mapping as the
// geatsc core emitter's uriBuiltinRuntimeName). The store-method lowering path
// would otherwise emit a bare `fn_encodeURIComponent` that has no declaration.
const URI_BUILTIN_RUNTIME: Record<string, string> = {
  decodeURI: 'gea::uri::decode',
  decodeURIComponent: 'gea::uri::decode_component',
  encodeURI: 'gea::uri::encode',
  encodeURIComponent: 'gea::uri::encode_component',
  escape: 'gea::uri::escape',
  unescape: 'gea::uri::unescape'
}

export interface GlobalStoreFieldTarget {
  access: string
  storeAccess: string
  fieldName: string
  hint: StoreMethodHint
}

export function globalStoreFieldTarget(context: StoreMethodLowerContext, expr: GeaIrStoreExpr): GlobalStoreFieldTarget | null {
  if (expr.kind !== 'member' || expr.object.kind !== 'identifier') return null
  const field = globalStoreFieldPlan(context, expr.object.name, expr.property)
  if (!field) return null
  const storeAccess = globalStoreAccess(expr.object.name)
  return {
    access: `${storeAccess}->${field.fieldName}`,
    storeAccess,
    fieldName: field.fieldName,
    hint: storeFieldHint(field.shape),
  }
}

function globalStoreFieldPlan(context: StoreMethodLowerContext, receiver: string, property: string) {
  if (!unshadowedGlobalStoreReceiver(context, receiver)) return null
  const receiverName = sanitizeCppIdentifier(receiver)
  const fieldName = sanitizeCppIdentifier(property)
  return context.storeFields.find((field) => field.storeGlobalName === receiverName && field.fieldName === fieldName) ?? null
}

function globalStoreMethodPlan(context: StoreMethodLowerContext, receiver: string, method: string) {
  if (!unshadowedGlobalStoreReceiver(context, receiver)) return null
  const receiverName = sanitizeCppIdentifier(receiver)
  const methodName = sanitizeCppIdentifier(method)
  return context.storeMethods.find((candidate) => candidate.storeGlobalName === receiverName && candidate.methodName === methodName) ?? null
}

function unshadowedGlobalStoreReceiver(context: StoreMethodLowerContext, receiver: string): boolean {
  if (context.locals.has(receiver) || context.params.has(receiver) || context.constants.has(receiver)) return false
  const receiverName = sanitizeCppIdentifier(receiver)
  return context.storeFields.some((field) => field.storeGlobalName === receiverName) || context.storeMethods.some((method) => method.storeGlobalName === receiverName)
}

function globalStoreAccess(receiver: string): string {
  return `__gea_global_${sanitizeCppIdentifier(receiver)}()`
}

function storeFieldHint(shape: StoreMethodLowerContext['storeFields'][number]['shape']): StoreMethodHint {
  if (shape?.kind === 'literal') {
    if (shape.valueType === 'boolean') return 'boolean'
    if (shape.valueType === 'number') return 'number'
    if (shape.valueType === 'string') return 'string'
  }
  return 'any'
}

function lowerIdentifier(context: StoreMethodLowerContext, name: string, hint: StoreMethodHint): string {
  const id = sanitizeCppIdentifier(name)
  if (context.constants.has(name)) return constantExpr(context.constants.get(name)!, hint)
  if (name === 'initBleServers') return context.topLevelFunctionNames.get(name) ?? 'fn_initBleServers'
  if (context.params.has(name)) {
    const paramHint = context.paramHints.get(name)
    if (hint === 'any' || paramHint === hint) return id
    if (hint === 'number') return `gea::runtime::coerce::to_number(${id})`
    if (hint === 'string') return `gea_cpp_to_string(${id})`
    if (hint === 'boolean') return `gea::runtime::coerce::to_boolean(${id})`
  }
  if (!context.locals.has(name)) {
    const topLevelValue = context.topLevelValueNames.get(name) ?? context.topLevelValueNames.get(id)
    if (topLevelValue) return coerceTopLevelValueForHint(topLevelValue, hint)
  }
  if (unshadowedGlobalStoreReceiver(context, name)) return globalStoreAccess(name)
  if (hint === 'string') return `gea_cpp_to_string(${id})`
  if (hint === 'boolean') return `gea::runtime::coerce::to_boolean(${id})`
  return id
}

function lowerMember(context: StoreMethodLowerContext, expr: Extract<GeaIrStoreExpr, { kind: 'member' }>, hint: StoreMethodHint): string | null {
  const path = exprPath(expr)
  if (path === 'Date.now') return 'gea_cpp_now_ms'
  if (path === 'audioContext.currentTime') return 'gea::host::sharedAudioContext().currentTime'
  if (path === '__gea_audioContext.currentTime') return 'gea::host::sharedAudioContext().currentTime'
  if (path === 'audioContext.destination') return 'gea::host::sharedAudioContext().destination'
  if (path === '__gea_audioContext.destination') return 'gea::host::sharedAudioContext().destination'
  if (path === 'Accelerometer.tiltX') return 'gea::host::Accelerometer.tiltX'
  if (path === 'Accelerometer.tiltY') return 'gea::host::Accelerometer.tiltY'
  if (path === 'Accelerometer.accelerationX') return 'gea::host::Accelerometer.accelerationX'
  if (path === 'Accelerometer.accelerationY') return 'gea::host::Accelerometer.accelerationY'
  if (path === 'Accelerometer.accelerationZ') return 'gea::host::Accelerometer.accelerationZ'
  if (path === 'Accelerometer.gyroscopeX') return 'gea::host::Accelerometer.gyroscopeX'
  if (path === 'Accelerometer.gyroscopeY') return 'gea::host::Accelerometer.gyroscopeY'
  if (path === 'Accelerometer.gyroscopeZ') return 'gea::host::Accelerometer.gyroscopeZ'
  if (path === 'Camera.width') return 'gea::host::Camera.width'
  if (path === 'Camera.height') return 'gea::host::Camera.height'
  if (path === 'Camera.orientation') return 'gea::host::Camera.orientation'
  if (path === 'Camera.facing') return 'gea::host::Camera.facing'
  if (path === 'Camera.deviceCount') return 'gea::host::Camera.deviceCount'

  // Viewport dimensions — browser-style `window.innerWidth`/`innerHeight`
  // and the framework's `Display.width`/`height`. Route through the host
  // Display facade so orientation changes and viewport metrics share one source.
  if (path === 'window.innerWidth' || path === 'Display.width' || path === 'display.width' || path === '__gea_Display.width') {
    return 'gea::host::Display.width()'
  }
  if (path === 'window.innerHeight' || path === 'Display.height' || path === 'display.height' || path === '__gea_Display.height') {
    return 'gea::host::Display.height()'
  }
  if (path === 'Display.nativeWidth' || path === 'display.nativeWidth' || path === '__gea_Display.nativeWidth') {
    return 'gea::host::Display.nativeWidth()'
  }
  if (path === 'Display.nativeHeight' || path === 'display.nativeHeight' || path === '__gea_Display.nativeHeight') {
    return 'gea::host::Display.nativeHeight()'
  }
  if (path === 'Display.orientation' || path === 'display.orientation' || path === '__gea_Display.orientation') {
    return 'gea::host::Display.orientation()'
  }
  if (path === 'Display.supportedOrientations' || path === 'display.supportedOrientations' || path === '__gea_Display.supportedOrientations') {
    return 'gea::host::Display.supportedOrientations()'
  }
  if (path === 'Display.autoRotate' || path === 'display.autoRotate' || path === '__gea_Display.autoRotate') {
    return 'gea::host::Display.autoRotate()'
  }
  if (path === 'Display.pixelFormat' || path === 'display.pixelFormat' || path === '__gea_Display.pixelFormat') {
    return 'gea::host::Display.pixelFormat()'
  }
  if (path === 'Display.panelPixelFormat' || path === 'display.panelPixelFormat' || path === '__gea_Display.panelPixelFormat') {
    return 'gea::host::Display.panelPixelFormat()'
  }
  if (path === 'Display.supportedPixelFormats' || path === 'display.supportedPixelFormats' || path === '__gea_Display.supportedPixelFormats') {
    return 'gea::host::Display.supportedPixelFormats()'
  }

  // Property access on `Display` — the framework's DisplayController
  // imported from `gea-embedded`. The TS interface has getters (`ctx`)
  // and methods, but on the C++ side `gea::host::DisplayFacade` exposes
  // *everything* as a method (including `width()`/`height()`/`ctx()`).
  // Emit with parens, matching what the top-level emitter does for
  // `hostNamespaceProperty` access (see expression-core.ts ~line 792).
  // Method calls like `Display.setFrameRate(...)` are handled separately
  // in `lowerHostFacadeCall` below, so this only fires for bare reads.
  // TODO: long-term, wire this emit path through the shared `hostNamespace*`
  // registry in `geatsc/src/host-shims.ts` so plugins don't need to enumerate
  // host facades twice (once for top-level, once for class methods).
  if (expr.object.kind === 'identifier' && (expr.object.name === 'Display' || expr.object.name === 'display' || expr.object.name === '__gea_Display')) {
    return `gea::host::Display.${sanitizeCppIdentifier(expr.property)}()`
  }

  if (expr.property === 'length' && expr.object.kind === 'identifier' && localArrayCppType(context, expr.object.name)) {
    return `static_cast<double>(${sanitizeCppIdentifier(expr.object.name)}.size())`
  }

  const lengthField = thisArrayLengthTarget(expr)
  if (lengthField) return `static_cast<double>((*this).${sanitizeCppIdentifier(lengthField)}.size())`

  const field = thisFieldName(expr)
  if (field) {
    const value = `(*this).${sanitizeCppIdentifier(field)}`
    return coerceForHint(value, hint)
  }

  const globalField = globalStoreFieldTarget(context, expr)
  if (globalField) {
    return hint === 'any' || globalField.hint === 'any' || hint === globalField.hint
      ? globalField.access
      : coerceForHint(globalField.access, hint)
  }

  // Read-only numeric DOM/element properties the host exposes as record
  // literals (e.g. a <virtual-list>'s scrollTop and measured rowHeight). Lower
  // them to a dynamic record read coerced to number, rather than a C++ member
  // access on the gea_cpp_value.
  if (expr.property === 'scrollTop' || expr.property === 'rowHeight') {
    const object = lowerExpr(context, expr.object, 'any')
    return object ? `gea::runtime::coerce::to_number(gea_cpp_key(${object}).record_get_literal("${expr.property}"))` : null
  }

  const item = thisArrayItemFieldTarget(expr)
  if (item) {
    const alias = item.index.kind === 'identifier' ? context.arrayItemFieldAliases.get(arrayItemFieldAliasKey(item.arrayField, item.index.name, item.itemField)) : undefined
    if (alias) return alias
    const index = lowerExpr(context, item.index, 'number')
    if (!index) return null
    const array = `(*this).${sanitizeCppIdentifier(item.arrayField)}`
    const field = JSON.stringify(item.itemField)
    const indexTemp = nextStoreMethodTemp(context, 'index')
    const directAccess = typedArrayItemFieldAccess(context.store, item.arrayField, item.itemField, `${array}[${indexTemp}]`)
    if (directAccess) {
      // Pick the lambda's return type from the FIELD's declared type, not the
      // caller's hint: a string field read as a bare method argument or in a
      // concatenation arrives with hint 'any'/'number', and a `-> double` lambda
      // returning the std::string member fails to compile. Coerce to the caller's
      // hint afterwards (e.g. number field in a string concat -> gea_cpp_to_string).
      const itemHint = arrayItemFieldHint(context.store, item.arrayField, item.itemField)
      let lambda: string
      if (itemHint === 'any') {
        const member = sanitizeCppIdentifier(item.itemField)
        const fieldType = `std::decay_t<decltype((${array})[0].${member})>`
        lambda = `([&]() -> ${fieldType} { using __GeaField = ${fieldType}; auto ${indexTemp} = static_cast<std::size_t>(${index}); if (${indexTemp} >= ${array}.size()) return __GeaField{}; return ${directAccess}; })()`
      } else if (itemHint === 'string') {
        lambda = `([&]() -> std::string { auto ${indexTemp} = static_cast<std::size_t>(${index}); if (${indexTemp} >= ${array}.size()) return std::string(); return ${directAccess}; })()`
      } else if (itemHint === 'boolean') {
        lambda = `([&]() -> bool { auto ${indexTemp} = static_cast<std::size_t>(${index}); if (${indexTemp} >= ${array}.size()) return false; return gea::runtime::coerce::to_boolean(${directAccess}); })()`
      } else {
        lambda = `([&]() -> double { auto ${indexTemp} = static_cast<std::size_t>(${index}); if (${indexTemp} >= ${array}.size()) return 0.0; return ${directAccess}; })()`
      }
      return hint === 'any' || hint === itemHint ? lambda : coerceForHint(lambda, hint)
    }
    if (hint === 'string') {
      return `([&]() -> std::string { auto ${indexTemp} = static_cast<std::size_t>(${index}); if (${indexTemp} >= ${array}.size()) return gea_ir::read_string_field(gea_cpp_value::missing(), ${field}); return gea_ir::read_string_field(${array}[${indexTemp}], ${field}); })()`
    }
    if (hint === 'boolean') {
      return `([&]() -> bool { auto ${indexTemp} = static_cast<std::size_t>(${index}); if (${indexTemp} >= ${array}.size()) return false; return gea::runtime::coerce::to_boolean(${array}[${indexTemp}].record_get_literal(${field})); })()`
    }
    return `([&]() -> double { auto ${indexTemp} = static_cast<std::size_t>(${index}); if (${indexTemp} >= ${array}.size()) return gea_ir::read_number_field(gea_cpp_value::missing(), ${field}); return gea_ir::read_number_field(${array}[${indexTemp}], ${field}); })()`
  }

  const aliasField = arrayElementAliasFieldTarget(context, expr)
  if (aliasField) {
    const { alias, itemField } = aliasField
    const itemHint = arrayItemFieldHint(context.store, alias.arrayField, itemField)
    const directAccess = typedArrayItemFieldAccess(context.store, alias.arrayField, itemField, alias.itemExpression)
    if (directAccess) {
      return hint === 'any' || hint === itemHint ? directAccess : coerceForHint(directAccess, hint)
    }
    const field = JSON.stringify(itemField)
    if (hint === 'string') return `gea_ir::read_string_field(${alias.itemExpression}, ${field})`
    if (hint === 'boolean') return `gea::runtime::coerce::to_boolean(${alias.itemExpression}.record_get_literal(${field}))`
    return `gea_ir::read_number_field(${alias.itemExpression}, ${field})`
  }

  // `localStorage.length` — the receiver is the host global, not a C++ value, so
  // it can't go through the generic length helper below (which would reference an
  // undeclared `localStorage`). Read the facade's count member directly.
  if (
    expr.property === 'length' &&
    expr.object.kind === 'identifier' &&
    expr.object.name === 'localStorage' &&
    !context.locals.has('localStorage') &&
    !context.params.has('localStorage') &&
    !context.constants.has('localStorage')
  ) {
    return coerceForHint('gea::host::Storage.length', hint)
  }

  if (expr.property === 'length') {
    const object = lowerExpr(context, expr.object, 'any')
    if (!object) return null
    return coerceForHint(
      `([&](const auto &__gea_v) -> double { using __GeaLenValue = std::decay_t<decltype(__gea_v)>; if constexpr (std::is_same_v<__GeaLenValue, std::string>) { return static_cast<double>(gea_cpp_string_utf16_length(__gea_v)); } else if constexpr (requires { __gea_v.size(); }) { return static_cast<double>(__gea_v.size()); } else { return static_cast<double>(gea_cpp_string_utf16_length(gea_cpp_to_string(__gea_v))); } })(${object})`,
      hint
    )
  }

  // A method param without a primitive hint arrives in the generated signature
  // as boxed gea_cpp_value — raw `.member` access on it is a miscompile (the
  // C++ value type has no such member). Bail the method to the generic
  // fallback rather than emit invalid code.
  if (expr.object.kind === 'identifier' && context.params.has(expr.object.name) && !context.paramHints.has(expr.object.name)) {
    return null
  }
  const object = lowerExpr(context, expr.object, 'any')
  return object ? coerceForHint(adaptiveMemberRead(object, sanitizeCppIdentifier(expr.property)), hint) : null
}

// Generic member access must survive both value receivers and heap-promoted
// handles: a module-global class cell can be refreshed to std::shared_ptr /
// gea_gc_ptr after classification, and a hard-coded `.member` on that handle
// either fails to compile or (worse) reads a stale value copy. Dispatch on
// operator-> structurally.
function adaptiveMemberRead(object: string, property: string): string {
  return `([&](auto &&__gea_mr_obj) -> decltype(auto) { if constexpr (requires { __gea_mr_obj->${property}; }) { return (__gea_mr_obj->${property}); } else { return (__gea_mr_obj.${property}); } })(${object})`
}

function lowerIndex(context: StoreMethodLowerContext, expr: Extract<GeaIrStoreExpr, { kind: 'index' }>, hint: StoreMethodHint): string | null {
  const thisArray = thisFieldName(expr.object)
  const index = lowerExpr(context, expr.index, 'number')
  if (!index) return null
  if (thisArray) {
    const alias = expr.index.kind === 'identifier' ? context.arrayItemAliases.get(arrayItemAliasKey(thisArray, expr.index.name)) : undefined
    if (alias) return alias
    const array = `(*this).${sanitizeCppIdentifier(thisArray)}`
    if (hint === 'boolean') {
      // JS truthiness of `this.arr[k]`: an out-of-bounds index reads `undefined`
      // (falsy), and a present element follows ToBoolean. A typed record-struct
      // element is a JS object → always truthy; decide that at compile time via
      // the `__gea_from_value` discriminator (record-alias structs declare it;
      // primitives, std::string, and gea_cpp_value do not) so the hot path needs
      // no gea_cpp_value allocation. Without this, the whole-element read below
      // returns a default-constructed struct for an OOB index, which `operator
      // bool` reports as truthy — so `if (!this.arr[k])` never fired for missing
      // indices, diverging from JS (where `arr[k]` is `undefined`).
      const indexTemp = nextStoreMethodTemp(context, 'index')
      return `([&]() -> bool { using __GeaItem = std::decay_t<decltype((${array})[0])>; auto ${indexTemp} = static_cast<std::size_t>(${index}); if (${indexTemp} >= ${array}.size()) return false; if constexpr (requires(const gea_cpp_value &__gea_v) { __GeaItem::__gea_from_value(__gea_v); }) return true; else return gea::runtime::coerce::to_boolean(${array}[${indexTemp}]); })()`
    }
    // Return BY VALUE in the array's ACTUAL element type — NOT a forced
    // `gea_cpp_value`. For a typed store array (`CityState[]`) the element is the
    // typed item struct, and erasing a whole-element read to `gea_cpp_value`
    // throws away the static type (so `cityCoordsMatch(this.cities[i])` no longer
    // sees a `CityState`). Deduce the element type via decltype so this works for
    // both typed and `gea_cpp_value` arrays; the out-of-bounds fallback is a
    // default-constructed element of that same type. By value (not const&) so a
    // bound reference can't dangle on the temporary the element materializes into.
    const indexTemp = nextStoreMethodTemp(context, 'index')
    // Coerce the by-value element read to the requested hint: a numeric element
    // used in a string concat (`'CC' + this.slotCc[i]`) must stringify via
    // gea_cpp_to_string, exactly like any other numeric operand in that context.
    // 'any'/'number' are no-ops, so typed-struct whole-element reads are
    // unaffected; the 'boolean' hint already returned above.
    return coerceForHint(
      `([&]() -> std::decay_t<decltype((${array})[0])> { using __GeaItem = std::decay_t<decltype((${array})[0])>; auto ${indexTemp} = static_cast<std::size_t>(${index}); if (${indexTemp} >= ${array}.size()) return __GeaItem{}; return ${array}[${indexTemp}]; })()`,
      hint,
    )
  }
  const object = lowerExpr(context, expr.object, 'any')
  return object ? `${object}[static_cast<std::size_t>(${index})]` : null
}

function lowerCall(context: StoreMethodLowerContext, expr: Extract<GeaIrStoreExpr, { kind: 'call' }>, hint: StoreMethodHint): string | null {
  const path = exprPath(expr.callee)
  const member = expr.callee.kind === 'member' ? expr.callee : null
  if (member?.property === 'startsWith') {
    const object = lowerExpr(context, member.object, 'string')
    const needle = expr.args[0] ? lowerExpr(context, expr.args[0], 'string') : 'std::string("undefined")'
    const position = expr.args[1] ? lowerExpr(context, expr.args[1], 'number') : '0.0'
    return object && needle && position
      ? coerceForHint(
          `([&](const std::string &__s, const std::string &__needle) -> bool { double __pos_d = ${position}; if (!std::isfinite(__pos_d) || __pos_d < 0.0) __pos_d = 0.0; if (__pos_d > static_cast<double>(__s.size())) __pos_d = static_cast<double>(__s.size()); auto __pos = static_cast<std::size_t>(__pos_d); if (__pos + __needle.size() > __s.size()) return false; return __s.compare(__pos, __needle.size(), __needle) == 0; })(${object}, ${needle})`,
          hint
        )
      : null
  }
  if (member?.property === 'endsWith') {
    const object = lowerExpr(context, member.object, 'string')
    const needle = expr.args[0] ? lowerExpr(context, expr.args[0], 'string') : 'std::string("undefined")'
    const endPosition = expr.args[1] ? lowerExpr(context, expr.args[1], 'number') : 'static_cast<double>(__s.size())'
    return object && needle && endPosition
      ? coerceForHint(
          `([&](const std::string &__s, const std::string &__needle) -> bool { double __end_d = ${endPosition}; if (!std::isfinite(__end_d) || __end_d > static_cast<double>(__s.size())) __end_d = static_cast<double>(__s.size()); if (__end_d < 0.0) __end_d = 0.0; auto __end = static_cast<std::size_t>(__end_d); if (__needle.size() > __end) return false; return __s.compare(__end - __needle.size(), __needle.size(), __needle) == 0; })(${object}, ${needle})`,
          hint
        )
      : null
  }
  if (member?.property === 'indexOf') {
    const object = lowerExpr(context, member.object, 'string')
    const needle = expr.args[0] ? lowerExpr(context, expr.args[0], 'string') : null
    const start = expr.args[1] ? lowerExpr(context, expr.args[1], 'number') : '0'
    return object && needle && start
      ? `([&](const std::string &__s) -> double { auto __start = static_cast<std::size_t>(std::max(0.0, ${start})); auto __pos = __s.find(${needle}, __start); return __pos == std::string::npos ? -1.0 : static_cast<double>(__pos); })(${object})`
      : null
  }
  if (member?.property === 'substring') {
    const object = lowerExpr(context, member.object, 'string')
    const start = expr.args[0] ? lowerExpr(context, expr.args[0], 'number') : '0'
    const end = expr.args[1] ? lowerExpr(context, expr.args[1], 'number') : null
    if (!object || !start) return null
    const endInit = end ? `std::min(__size, static_cast<std::size_t>(std::max(0.0, ${end})))` : '__size'
    return `([&](const std::string &__s) -> std::string { auto __size = __s.size(); auto __start = std::min(__size, static_cast<std::size_t>(std::max(0.0, ${start}))); auto __end = ${endInit}; if (__end < __start) std::swap(__start, __end); return __s.substr(__start, __end - __start); })(${object})`
  }
  if (path === 'JSON.parse' || path === 'JSON.stringify') return null
  if (path === 'Date.now') return coerceForHint('gea_cpp_now_ms()', hint)
  if (path === 'Math.random') return coerceForHint('gea::runtime::math::random()', hint)
  if (path === 'Math.floor') {
    const arg = expr.args[0] ? lowerExpr(context, expr.args[0], 'number') : null
    return arg ? coerceForHint(`std::floor(${arg})`, hint) : null
  }
  if (path === 'Math.round') {
    const arg = expr.args[0] ? lowerExpr(context, expr.args[0], 'number') : null
    return arg ? coerceForHint(`std::floor((${arg}) + 0.5)`, hint) : null
  }
  // The rest of the common Math intrinsics. Anything missing from this table
  // falls through to an emission that references a bare `Math` symbol the C++
  // has no definition for ('Math' was not declared in this scope) — hit first
  // by Math.ceil in the e-reader's Contents pager math.
  if (path === 'Math.ceil' || path === 'Math.trunc' || path === 'Math.sqrt' ||
      path === 'Math.sin' || path === 'Math.cos' || path === 'Math.tan' ||
      path === 'Math.log' || path === 'Math.exp') {
    const fn = path.slice('Math.'.length)
    const cppFn = fn === 'log' ? 'std::log' : `std::${fn}`
    const arg = expr.args[0] ? lowerExpr(context, expr.args[0], 'number') : null
    return arg ? coerceForHint(`${cppFn}(${arg})`, hint) : null
  }
  if (path === 'Math.abs') {
    const arg = expr.args[0] ? lowerExpr(context, expr.args[0], 'number') : null
    return arg ? coerceForHint(`std::fabs(${arg})`, hint) : null
  }
  if (path === 'Math.pow' || path === 'Math.atan2') {
    const a = expr.args[0] ? lowerExpr(context, expr.args[0], 'number') : null
    const b = expr.args[1] ? lowerExpr(context, expr.args[1], 'number') : null
    const cppFn = path === 'Math.pow' ? 'std::pow' : 'std::atan2'
    return a && b ? coerceForHint(`${cppFn}(${a}, ${b})`, hint) : null
  }
  if (path === 'Math.min' || path === 'Math.max') {
    if (expr.args.length === 0) return null
    const args: string[] = []
    for (const argExpr of expr.args) {
      const lowered = lowerExpr(context, argExpr, 'number')
      if (!lowered) return null
      args.push(lowered)
    }
    if (args.length === 1) return coerceForHint(`(${args[0]})`, hint)
    const cppFn = path === 'Math.min' ? 'std::min<double>' : 'std::max<double>'
    return coerceForHint(`${cppFn}({${args.join(', ')}})`, hint)
  }
  if (path === 'Math.sign') {
    const arg = expr.args[0] ? lowerExpr(context, expr.args[0], 'number') : null
    return arg
      ? coerceForHint(`([&](double __gea_value) -> double { return __gea_value > 0.0 ? 1.0 : (__gea_value < 0.0 ? -1.0 : __gea_value); })(${arg})`, hint)
      : null
  }
  if (path === 'Number.isInteger') {
    const arg = expr.args[0] ? lowerExpr(context, expr.args[0], 'number') : null
    return arg
      ? coerceForHint(`([&](double __gea_value) -> bool { return std::isfinite(__gea_value) && std::floor(__gea_value) == __gea_value; })(${arg})`, hint)
      : null
  }
  // The primitive coercion globals — `Number(x)` / `String(x)` / `Boolean(x)` —
  // turn one value into a number/string/boolean (e.g. parsing a localStorage
  // string back to a number). They work in free functions and templates, so wire
  // them up for store methods too (they are deliberately excluded from the
  // top-level-function fallback in shouldLowerAsTopLevelFunction).
  if (
    expr.callee.kind === 'identifier' &&
    expr.args.length === 1 &&
    (expr.callee.name === 'Number' || expr.callee.name === 'String' || expr.callee.name === 'Boolean') &&
    !context.locals.has(expr.callee.name) &&
    !context.params.has(expr.callee.name) &&
    !context.constants.has(expr.callee.name)
  ) {
    const inner = lowerExpr(context, expr.args[0], 'any')
    if (!inner) return null
    if (expr.callee.name === 'Number') return coerceForHint(`gea::runtime::coerce::to_number(${inner})`, hint)
    if (expr.callee.name === 'String') return coerceForHint(`gea_cpp_to_string(${inner})`, hint)
    return coerceForHint(`gea::runtime::coerce::to_boolean(${inner})`, hint)
  }
  if (member?.property === 'toFixed') {
    const object = lowerExpr(context, member.object, 'number')
    const digits = expr.args[0] ? lowerExpr(context, expr.args[0], 'number') : '0.0'
    if (!object || !digits) return null
    const call = `geaStoreNumberToFixed(${object}, ${digits})`
    if (hint === 'boolean') return `gea::runtime::coerce::to_boolean(${call})`
    if (hint === 'number') return `gea::runtime::coerce::to_number(${call})`
    return call
  }
  if (path === 'audioContext.createOscillator') return coerceForHint('gea::host::sharedAudioContext().createOscillator()', hint)
  if (path === '__gea_audioContext.createOscillator') return coerceForHint('gea::host::sharedAudioContext().createOscillator()', hint)
  if (path === 'Accelerometer.start') return coerceForHint('gea::host::Accelerometer.start()', hint)
  if (path === 'touch.read' || path === 'touch.__gea_read') {
    const sample = 'gea::host::touch.read()'
    return hint === 'any' ? sample : coerceForHint(sample, hint)
  }
  if (path === 'document.getElementById') {
    const id = expr.args[0] ? lowerExpr(context, expr.args[0], 'string') : null
    return id ? `gea::runtime::host::document().getElementById(${id})` : null
  }
  // querySelector / querySelectorAll: emit the TYPED embedded-Document form (the
  // same one the main emitter uses) so the result is a NodeHandle /
  // std::vector<NodeHandle> with real methods (scrollIntoView, size, indexing).
  // The store-method lowerer's generic member-call path would otherwise wrap this
  // in a pointer-guard IIFE that leaves the `document` global unresolved.
  if (path === 'document.querySelector') {
    const sel = expr.args[0] ? lowerExpr(context, expr.args[0], 'string') : null
    return sel ? `gea::embedded::ui::Document::instance().querySelector((${sel}).c_str())` : null
  }
  if (path === 'document.querySelectorAll') {
    const sel = expr.args[0] ? lowerExpr(context, expr.args[0], 'string') : null
    return sel ? `gea::embedded::ui::Document::instance().querySelectorAll((${sel}).c_str())` : null
  }
  const hostCall = lowerHostFacadeCall(context, path, expr.args)
  if (hostCall) return coerceForHint(hostCall, hint)
  if (path === 'console.log') {
    const parts = expr.args.map((arg) => lowerExpr(context, arg, 'string'))
    return parts.every(Boolean) ? `gea::runtime::console::log(${parts.join(' + ')})` : null
  }
  const pushField = thisArrayPushTarget(expr.callee)
  if (pushField) {
    context.needsArrayPush = true
    const args = expr.args.map((arg) => lowerTypedElementLiteral(context, pushField, arg) ?? lowerExpr(context, arg, 'any'))
    return args.every(Boolean) ? `push_array_items(${JSON.stringify(pushField)}, (*this).${sanitizeCppIdentifier(pushField)}${args.length > 0 ? `, ${args.join(', ')}` : ''})` : null
  }
  const unshiftField = thisArrayUnshiftTarget(expr.callee)
  if (unshiftField) {
    context.needsArrayUnshift = true
    const args = expr.args.map((arg) => lowerTypedElementLiteral(context, unshiftField, arg) ?? lowerExpr(context, arg, 'any'))
    return args.every(Boolean) ? `unshift_array_items(${JSON.stringify(unshiftField)}, (*this).${sanitizeCppIdentifier(unshiftField)}${args.length > 0 ? `, ${args.join(', ')}` : ''})` : null
  }
  const spliceField = thisArraySpliceTarget(expr.callee)
  if (spliceField) {
    context.needsArraySplice = true
    if (expr.args.length < 1) return null
    const start = lowerExpr(context, expr.args[0], 'number')
    if (!start) return null
    const deleteCount = expr.args.length > 1 ? lowerExpr(context, expr.args[1], 'number') : '0'
    if (!deleteCount) return null
    const inserts = expr.args.slice(2).map((arg) => lowerTypedElementLiteral(context, spliceField, arg) ?? lowerExpr(context, arg, 'any'))
    if (!inserts.every(Boolean)) return null
    const insertList = inserts.length > 0 ? `, ${inserts.join(', ')}` : ''
    return `splice_array_items(${JSON.stringify(spliceField)}, (*this).${sanitizeCppIdentifier(spliceField)}, ${start}, ${deleteCount}${insertList})`
  }
  const popField = thisArrayPopTarget(expr.callee)
  if (popField) {
    context.needsArrayPop = true
    if (expr.args.length !== 0) return null
    return `pop_array_item(${JSON.stringify(popField)}, (*this).${sanitizeCppIdentifier(popField)})`
  }
  const shiftField = thisArrayShiftTarget(expr.callee)
  if (shiftField) {
    context.needsArrayShift = true
    if (expr.args.length !== 0) return null
    return `shift_array_item(${JSON.stringify(shiftField)}, (*this).${sanitizeCppIdentifier(shiftField)})`
  }
  if (member?.property === 'push' && member.object.kind === 'identifier') {
    const localName = member.object.name
    const elementType = localArrayTypeRef(context, localName)
    if (elementType) {
      const vectorName = sanitizeCppIdentifier(localName)
      // Lower the pushed value with the element's own hint so a primitive
      // vector (double/std::string/bool) gets a concretely-typed expression
      // instead of a boxed `any`.
      const elementHint =
        elementType === 'double' ? 'number' : elementType === 'std::string' ? 'string' : elementType === 'bool' ? 'boolean' : 'any'
      const args = expr.args.map((arg) => lowerElementLiteralForType(context, elementType, arg) ?? lowerExpr(context, arg, elementHint))
      if (!args.every(Boolean)) return null
      const pushes = args.map((arg) => `${vectorName}.push_back(${arg});`).join(' ')
      return `([&]() -> double { ${pushes} return static_cast<double>(${vectorName}.size()); })()`
    }
  }
  if (expr.callee.kind === 'identifier' && URI_BUILTIN_RUNTIME[expr.callee.name] && expr.args.length === 1) {
    const arg = lowerExpr(context, expr.args[0], 'string')
    return arg ? coerceForHint(`${URI_BUILTIN_RUNTIME[expr.callee.name]}(${arg})`, hint) : null
  }
  if (expr.callee.kind === 'identifier' && !context.locals.has(expr.callee.name) && !context.params.has(expr.callee.name)) {
    const calleeName = sanitizeCppIdentifier(expr.callee.name)
    const topLevelValue = context.topLevelValueNames.get(expr.callee.name) ?? context.topLevelValueNames.get(calleeName)
    if (topLevelValue) {
      const args = expr.args.map((arg) => lowerExpr(context, arg, 'any'))
      return args.every(Boolean) ? coerceForHint(`${topLevelValue}(${args.join(', ')})`, hint) : null
    }
  }
  if (expr.callee.kind === 'identifier' && shouldLowerAsTopLevelFunction(context, expr.callee.name)) {
    const args = expr.args.map((arg) => lowerExpr(context, arg, 'any'))
    const fnName = context.topLevelFunctionNames.get(expr.callee.name) ?? `fn_${sanitizeCppIdentifier(expr.callee.name)}`
    return args.every(Boolean) ? coerceTopLevelFunctionCallForHint(`${fnName}(${args.join(', ')})`, hint) : null
  }
  if (member?.object.kind === 'identifier') {
    const method = globalStoreMethodPlan(context, member.object.name, member.property)
    if (method && method.params.length === expr.args.length) {
      const args = expr.args.map((arg, index) => lowerExpr(context, arg, method.params[index]?.valueType ?? 'any'))
      return args.every(Boolean)
        ? coerceForHint(`${globalStoreAccess(member.object.name)}->${method.methodName}(${args.join(', ')})`, hint)
        : null
    }
  }
  if (member?.object.kind === 'this') {
    const args = expr.args.map((arg) => lowerExpr(context, arg, 'any'))
    return args.every(Boolean) ? `this->${sanitizeCppIdentifier(member.property)}(${args.join(', ')})` : null
  }
  if (member) {
    // Same pointer-agnostic dispatch as adaptiveMemberRead, but for CALLS: the
    // member-function name can't be returned from a lambda, so the whole call
    // expression sits inside the constexpr branch.
    const object = lowerExpr(context, member.object, 'any')
    const args = expr.args.map((arg) => lowerExpr(context, arg, 'any'))
    if (object && args.every(Boolean)) {
      const prop = sanitizeCppIdentifier(member.property)
      const argList = args.join(', ')
      return coerceForHint(
        `([&](auto &&__gea_mc_obj) -> decltype(auto) { if constexpr (requires { __gea_mc_obj->${prop}(${argList}); }) { return __gea_mc_obj->${prop}(${argList}); } else { return __gea_mc_obj.${prop}(${argList}); } })(${object})`,
        hint
      )
    }
    return null
  }
  const callee = lowerExpr(context, expr.callee, 'any')
  const args = expr.args.map((arg) => lowerExpr(context, arg, hint))
  return callee && args.every(Boolean) ? coerceForHint(`${callee}(${args.join(', ')})`, hint) : null
}

function shouldLowerAsTopLevelFunction(context: StoreMethodLowerContext, name: string): boolean {
  if (context.locals.has(name) || context.params.has(name) || context.constants.has(name)) return false
  return !new Set([
    'Number',
    'String',
    'Boolean',
    'fetch',
    'setTimeout',
    'clearTimeout',
    'setInterval',
    'clearInterval',
    'requestAnimationFrame',
  ]).has(name)
}

function coerceTopLevelFunctionCallForHint(call: string, hint: StoreMethodHint): string {
  if (hint === 'number') return `gea::runtime::coerce::to_number(${call})`
  return coerceForHint(call, hint)
}

function coerceTopLevelValueForHint(value: string, hint: StoreMethodHint): string {
  if (hint === 'number') return `gea::runtime::coerce::to_number(${value})`
  return coerceForHint(value, hint)
}

function lowerHostFacadeCall(context: StoreMethodLowerContext, path: string | null, args: GeaIrStoreExpr[]): string | null {
  if (!path) return null
  const calls: Record<string, { target: string; hints?: StoreMethodHint[]; optionsRecord?: boolean }> = {
    'Apps.launch': { target: 'gea::host::apps.launch', hints: ['string'] },
    'apps.launch': { target: 'gea::host::apps.launch', hints: ['string'] },

    'Geolocation.hasFix': { target: 'gea::host::navigator.geolocation.hasFix' },
    'geolocation.hasFix': { target: 'gea::host::navigator.geolocation.hasFix' },
    'Geolocation.currentPosition': { target: 'gea::host::navigator.geolocation.currentPosition' },
    'geolocation.currentPosition': { target: 'gea::host::navigator.geolocation.currentPosition' },
    'Geolocation.coords': { target: 'gea::host::navigator.geolocation.coords' },
    'geolocation.coords': { target: 'gea::host::navigator.geolocation.coords' },
    'Geolocation.latitude': { target: 'gea::host::navigator.geolocation.latitude' },
    'geolocation.latitude': { target: 'gea::host::navigator.geolocation.latitude' },
    'Geolocation.longitude': { target: 'gea::host::navigator.geolocation.longitude' },
    'geolocation.longitude': { target: 'gea::host::navigator.geolocation.longitude' },
    'Geolocation.accuracy': { target: 'gea::host::navigator.geolocation.accuracy' },
    'geolocation.accuracy': { target: 'gea::host::navigator.geolocation.accuracy' },

    'Audio.getVolume': { target: 'gea::host::Audio.getVolume' },
    '__gea_Audio.getVolume': { target: 'gea::host::Audio.getVolume' },
    'Audio.setVolume': { target: 'gea::host::Audio.setVolume', hints: ['number'] },
    '__gea_Audio.setVolume': { target: 'gea::host::Audio.setVolume', hints: ['number'] },

    'WiFi.enabled': { target: 'gea::host::navigator.wifi.enabled' },
    'wifi.enabled': { target: 'gea::host::navigator.wifi.enabled' },
    'WiFi.setEnabled': { target: 'gea::host::navigator.wifi.setEnabled', hints: ['boolean'] },
    'wifi.setEnabled': { target: 'gea::host::navigator.wifi.setEnabled', hints: ['boolean'] },
    'WiFi.connected': { target: 'gea::host::navigator.wifi.connected' },
    'wifi.connected': { target: 'gea::host::navigator.wifi.connected' },
    'WiFi.isConnected': { target: 'gea::host::navigator.wifi.connected' },
    'wifi.isConnected': { target: 'gea::host::navigator.wifi.connected' },
    'WiFi.ssid': { target: 'gea::host::navigator.wifi.ssid' },
    'wifi.ssid': { target: 'gea::host::navigator.wifi.ssid' },
    'WiFi.getSSID': { target: 'gea::host::navigator.wifi.ssid' },
    'wifi.getSSID': { target: 'gea::host::navigator.wifi.ssid' },
    'WiFi.ip': { target: 'gea::host::navigator.wifi.ip' },
    'wifi.ip': { target: 'gea::host::navigator.wifi.ip' },
    'WiFi.getIP': { target: 'gea::host::navigator.wifi.ip' },
    'wifi.getIP': { target: 'gea::host::navigator.wifi.ip' },
    'WiFi.rssi': { target: 'gea::host::navigator.wifi.rssi' },
    'wifi.rssi': { target: 'gea::host::navigator.wifi.rssi' },
    'WiFi.getRSSI': { target: 'gea::host::navigator.wifi.rssi' },
    'wifi.getRSSI': { target: 'gea::host::navigator.wifi.rssi' },
    'WiFi.mac': { target: 'gea::host::navigator.wifi.mac' },
    'wifi.mac': { target: 'gea::host::navigator.wifi.mac' },
    'WiFi.configure': { target: 'gea::host::navigator.wifi.configure', hints: ['string', 'string'] },
    'wifi.configure': { target: 'gea::host::navigator.wifi.configure', hints: ['string', 'string'] },
    'WiFi.waitForConnection': { target: 'gea::host::navigator.wifi.waitForConnection', hints: ['number'] },
    'wifi.waitForConnection': { target: 'gea::host::navigator.wifi.waitForConnection', hints: ['number'] },
    'WiFi.startScan': { target: 'gea::host::navigator.wifi.startScan' },
    'wifi.startScan': { target: 'gea::host::navigator.wifi.startScan' },
    'WiFi.scanning': { target: 'gea::host::navigator.wifi.scanning' },
    'wifi.scanning': { target: 'gea::host::navigator.wifi.scanning' },
    'WiFi.scanCount': { target: 'gea::host::navigator.wifi.scanCount' },
    'wifi.scanCount': { target: 'gea::host::navigator.wifi.scanCount' },
    'WiFi.scanSsidAt': { target: 'gea::host::navigator.wifi.scanSsidAt', hints: ['number'] },
    'wifi.scanSsidAt': { target: 'gea::host::navigator.wifi.scanSsidAt', hints: ['number'] },
    'WiFi.scanRssiAt': { target: 'gea::host::navigator.wifi.scanRssiAt', hints: ['number'] },
    'wifi.scanRssiAt': { target: 'gea::host::navigator.wifi.scanRssiAt', hints: ['number'] },
    'WiFi.scanSecuredAt': { target: 'gea::host::navigator.wifi.scanSecuredAt', hints: ['number'] },
    'wifi.scanSecuredAt': { target: 'gea::host::navigator.wifi.scanSecuredAt', hints: ['number'] },

    'BLE.init': { target: 'gea::host::navigator.bluetooth.init', hints: ['string', 'number', 'string'] },
    'bluetooth.init': { target: 'gea::host::navigator.bluetooth.init', hints: ['string', 'number', 'string'] },
    'BLE.enabled': { target: 'gea::host::navigator.bluetooth.enabled' },
    'bluetooth.enabled': { target: 'gea::host::navigator.bluetooth.enabled' },
    'BLE.setEnabled': { target: 'gea::host::navigator.bluetooth.setEnabled', hints: ['boolean'] },
    'bluetooth.setEnabled': { target: 'gea::host::navigator.bluetooth.setEnabled', hints: ['boolean'] },
    'BLE.connected': { target: 'gea::host::navigator.bluetooth.connected' },
    'bluetooth.connected': { target: 'gea::host::navigator.bluetooth.connected' },
    'BLE.bound': { target: 'gea::host::navigator.bluetooth.bound' },
    'bluetooth.bound': { target: 'gea::host::navigator.bluetooth.bound' },
    'BLE.batteryLevel': { target: 'gea::host::navigator.bluetooth.batteryLevel' },
    'bluetooth.batteryLevel': { target: 'gea::host::navigator.bluetooth.batteryLevel' },
    'BLE.mac': { target: 'gea::host::navigator.bluetooth.mac' },
    'bluetooth.mac': { target: 'gea::host::navigator.bluetooth.mac' },
    'BLE.deviceName': { target: 'gea::host::navigator.bluetooth.deviceName' },
    'bluetooth.deviceName': { target: 'gea::host::navigator.bluetooth.deviceName' },
    'BLE.startAdvertising': { target: 'gea::host::navigator.bluetooth.startAdvertising' },
    'bluetooth.startAdvertising': { target: 'gea::host::navigator.bluetooth.startAdvertising' },
    'BLE.stopAdvertising': { target: 'gea::host::navigator.bluetooth.stopAdvertising' },
    'bluetooth.stopAdvertising': { target: 'gea::host::navigator.bluetooth.stopAdvertising' },
    'BLE.keyboard.tap': { target: 'gea::host::navigator.bluetooth.keyboard.tap', hints: ['number'] },
    'bluetooth.keyboard.tap': { target: 'gea::host::navigator.bluetooth.keyboard.tap', hints: ['number'] },
    'BLE.keyboard.down': { target: 'gea::host::navigator.bluetooth.keyboard.down', hints: ['number', 'number'] },
    'bluetooth.keyboard.down': { target: 'gea::host::navigator.bluetooth.keyboard.down', hints: ['number', 'number'] },
    'BLE.keyboard.up': { target: 'gea::host::navigator.bluetooth.keyboard.up' },
    'bluetooth.keyboard.up': { target: 'gea::host::navigator.bluetooth.keyboard.up' },
    'BLE.mouse.move': { target: 'gea::host::navigator.bluetooth.mouse.move', hints: ['number', 'number', 'number', 'number'] },
    'bluetooth.mouse.move': { target: 'gea::host::navigator.bluetooth.mouse.move', hints: ['number', 'number', 'number', 'number'] },
    'BLE.mouse.click': { target: 'gea::host::navigator.bluetooth.mouse.click', hints: ['number'] },
    'bluetooth.mouse.click': { target: 'gea::host::navigator.bluetooth.mouse.click', hints: ['number'] },
    // BLE-MIDI (MIDI over GATT) — mirrors the `BLE.midi` entries in
    // host-shims.ts so class-method bodies resolve them the same way
    // top-level code does.
    'BLE.midi.enable': { target: 'gea::host::navigator.bluetooth.midi.enable' },
    'bluetooth.midi.enable': { target: 'gea::host::navigator.bluetooth.midi.enable' },
    'BLE.midi.bound': { target: 'gea::host::navigator.bluetooth.midi.bound' },
    'bluetooth.midi.bound': { target: 'gea::host::navigator.bluetooth.midi.bound' },
    'BLE.midi.send': { target: 'gea::host::navigator.bluetooth.midi.send' },
    'bluetooth.midi.send': { target: 'gea::host::navigator.bluetooth.midi.send' },
    'BLE.midi.startScan': { target: 'gea::host::navigator.bluetooth.midi.startScan' },
    'bluetooth.midi.startScan': { target: 'gea::host::navigator.bluetooth.midi.startScan' },
    'BLE.midi.stopScan': { target: 'gea::host::navigator.bluetooth.midi.stopScan' },
    'bluetooth.midi.stopScan': { target: 'gea::host::navigator.bluetooth.midi.stopScan' },
    'BLE.midi.scanning': { target: 'gea::host::navigator.bluetooth.midi.scanning' },
    'bluetooth.midi.scanning': { target: 'gea::host::navigator.bluetooth.midi.scanning' },
    'BLE.midi.scanCount': { target: 'gea::host::navigator.bluetooth.midi.scanCount' },
    'bluetooth.midi.scanCount': { target: 'gea::host::navigator.bluetooth.midi.scanCount' },
    'BLE.midi.scanNameAt': { target: 'gea::host::navigator.bluetooth.midi.scanNameAt', hints: ['number'] },
    'bluetooth.midi.scanNameAt': { target: 'gea::host::navigator.bluetooth.midi.scanNameAt', hints: ['number'] },
    'BLE.midi.connect': { target: 'gea::host::navigator.bluetooth.midi.connect', hints: ['number'] },
    'bluetooth.midi.connect': { target: 'gea::host::navigator.bluetooth.midi.connect', hints: ['number'] },
    'BLE.midi.disconnect': { target: 'gea::host::navigator.bluetooth.midi.disconnect' },
    'bluetooth.midi.disconnect': { target: 'gea::host::navigator.bluetooth.midi.disconnect' },

    // BLE HID-host role (this device is the central; a remote keyboard/macro
    // pad like the XPPen ACK05 is the input source) — mirrors `BLE.hidHost`
    // in host-shims.ts.
    'BLE.hidHost.startScan': { target: 'gea::host::navigator.bluetooth.hidHost.startScan' },
    'bluetooth.hidHost.startScan': { target: 'gea::host::navigator.bluetooth.hidHost.startScan' },
    'BLE.hidHost.stopScan': { target: 'gea::host::navigator.bluetooth.hidHost.stopScan' },
    'bluetooth.hidHost.stopScan': { target: 'gea::host::navigator.bluetooth.hidHost.stopScan' },
    'BLE.hidHost.scanning': { target: 'gea::host::navigator.bluetooth.hidHost.scanning' },
    'bluetooth.hidHost.scanning': { target: 'gea::host::navigator.bluetooth.hidHost.scanning' },
    'BLE.hidHost.scanCount': { target: 'gea::host::navigator.bluetooth.hidHost.scanCount' },
    'bluetooth.hidHost.scanCount': { target: 'gea::host::navigator.bluetooth.hidHost.scanCount' },
    'BLE.hidHost.scanNameAt': { target: 'gea::host::navigator.bluetooth.hidHost.scanNameAt', hints: ['number'] },
    'bluetooth.hidHost.scanNameAt': { target: 'gea::host::navigator.bluetooth.hidHost.scanNameAt', hints: ['number'] },
    'BLE.hidHost.connect': { target: 'gea::host::navigator.bluetooth.hidHost.connect', hints: ['number'] },
    'bluetooth.hidHost.connect': { target: 'gea::host::navigator.bluetooth.hidHost.connect', hints: ['number'] },
    'BLE.hidHost.disconnect': { target: 'gea::host::navigator.bluetooth.hidHost.disconnect' },
    'bluetooth.hidHost.disconnect': { target: 'gea::host::navigator.bluetooth.hidHost.disconnect' },
    'BLE.hidHost.bound': { target: 'gea::host::navigator.bluetooth.hidHost.bound' },
    'bluetooth.hidHost.bound': { target: 'gea::host::navigator.bluetooth.hidHost.bound' },
    'BLE.hidHost.reportCount': { target: 'gea::host::navigator.bluetooth.hidHost.reportCount' },
    'bluetooth.hidHost.reportCount': { target: 'gea::host::navigator.bluetooth.hidHost.reportCount' },
    'BLE.hidHost.reportIdAt': { target: 'gea::host::navigator.bluetooth.hidHost.reportIdAt', hints: ['number'] },
    'bluetooth.hidHost.reportIdAt': { target: 'gea::host::navigator.bluetooth.hidHost.reportIdAt', hints: ['number'] },
    'BLE.hidHost.reportLenAt': { target: 'gea::host::navigator.bluetooth.hidHost.reportLenAt', hints: ['number'] },
    'bluetooth.hidHost.reportLenAt': { target: 'gea::host::navigator.bluetooth.hidHost.reportLenAt', hints: ['number'] },
    'BLE.hidHost.reportByteAt': { target: 'gea::host::navigator.bluetooth.hidHost.reportByteAt', hints: ['number', 'number'] },
    'bluetooth.hidHost.reportByteAt': { target: 'gea::host::navigator.bluetooth.hidHost.reportByteAt', hints: ['number', 'number'] },
    'BLE.hidHost.clearReports': { target: 'gea::host::navigator.bluetooth.hidHost.clearReports' },
    'bluetooth.hidHost.clearReports': { target: 'gea::host::navigator.bluetooth.hidHost.clearReports' },

    // BLE connection registry — mirrors `BLE.connections` in host-shims.ts.
    'BLE.connections.count': { target: 'gea::host::navigator.bluetooth.connections.count' },
    'bluetooth.connections.count': { target: 'gea::host::navigator.bluetooth.connections.count' },
    'BLE.connections.kindAt': { target: 'gea::host::navigator.bluetooth.connections.kindAt', hints: ['number'] },
    'bluetooth.connections.kindAt': { target: 'gea::host::navigator.bluetooth.connections.kindAt', hints: ['number'] },
    'BLE.connections.nameAt': { target: 'gea::host::navigator.bluetooth.connections.nameAt', hints: ['number'] },
    'bluetooth.connections.nameAt': { target: 'gea::host::navigator.bluetooth.connections.nameAt', hints: ['number'] },

    // Config service (custom GATT) — a Web Bluetooth portal live-programs the
    // device. Mirrors `BLE.config` in host-shims.ts. setDocument takes a
    // number[] lowered to native std::vector<double> like BLE.midi.send (no
    // hints); the *At getters take a numeric index.
    'BLE.config.setDocument': { target: 'gea::host::navigator.bluetooth.config.setDocument' },
    'bluetooth.config.setDocument': { target: 'gea::host::navigator.bluetooth.config.setDocument' },
    'BLE.config.pendingLength': { target: 'gea::host::navigator.bluetooth.config.pendingLength' },
    'bluetooth.config.pendingLength': { target: 'gea::host::navigator.bluetooth.config.pendingLength' },
    'BLE.config.pendingByteAt': { target: 'gea::host::navigator.bluetooth.config.pendingByteAt', hints: ['number'] },
    'bluetooth.config.pendingByteAt': { target: 'gea::host::navigator.bluetooth.config.pendingByteAt', hints: ['number'] },
    'BLE.config.consumePending': { target: 'gea::host::navigator.bluetooth.config.consumePending' },
    'bluetooth.config.consumePending': { target: 'gea::host::navigator.bluetooth.config.consumePending' },
    'BLE.config.pairing': { target: 'gea::host::navigator.bluetooth.config.pairing' },
    'bluetooth.config.pairing': { target: 'gea::host::navigator.bluetooth.config.pairing' },
    'BLE.config.pairCode': { target: 'gea::host::navigator.bluetooth.config.pairCode' },
    'bluetooth.config.pairCode': { target: 'gea::host::navigator.bluetooth.config.pairCode' },
    'BLE.config.dismissPairing': { target: 'gea::host::navigator.bluetooth.config.dismissPairing' },
    'bluetooth.config.dismissPairing': { target: 'gea::host::navigator.bluetooth.config.dismissPairing' },
    // Device -> portal activity push. Takes a numeric control index.
    'BLE.config.pushActivity': { target: 'gea::host::navigator.bluetooth.config.pushActivity', hints: ['number'] },
    'bluetooth.config.pushActivity': { target: 'gea::host::navigator.bluetooth.config.pushActivity', hints: ['number'] },

    // DisplayController — the framework's `Display` import from `gea-embedded`.
    // Mirrors the `__gea_Display` entries in host-shims.ts so class-method
    // bodies resolve `Display.X` the same way top-level code does.
    'Display.getBrightness': { target: 'gea::host::Display.getBrightness' },
    'Display.setBrightness': { target: 'gea::host::Display.setBrightness', hints: ['number'] },
    'Display.getDevicePixelRatio': { target: 'gea::host::Display.getDevicePixelRatio' },
    'Display.setDevicePixelRatio': { target: 'gea::host::Display.setDevicePixelRatio', hints: ['number'] },
    'Display.getFrameIntervalMs': { target: 'gea::host::Display.getFrameIntervalMs' },
    'Display.setFrameIntervalMs': { target: 'gea::host::Display.setFrameIntervalMs', hints: ['number'] },
    'Display.getFrameRate': { target: 'gea::host::Display.getFrameRate' },
    'Display.setFrameRate': { target: 'gea::host::Display.setFrameRate', hints: ['number'] },
    'Display.getOrientation': { target: 'gea::host::Display.getOrientation' },
    'Display.setOrientation': { target: 'gea::host::Display.setOrientation', hints: ['string'] },
    'Display.getSupportedOrientations': { target: 'gea::host::Display.getSupportedOrientations' },
    'Display.setSupportedOrientations': { target: 'gea::host::Display.setSupportedOrientations' },
    'Display.getAutoRotate': { target: 'gea::host::Display.getAutoRotate' },
    'Display.setAutoRotate': { target: 'gea::host::Display.setAutoRotate', hints: ['boolean'] },
    'Display.setVSync': { target: 'gea::host::Display.setVSync', hints: ['boolean'] },
    'Display.setTextRasterCache': { target: 'gea::host::Display.setTextRasterCache', hints: ['boolean'] },
    'Display.setTextSolidBackdrop': { target: 'gea::host::Display.setTextSolidBackdrop', hints: ['number'] },
    'Display.invalidate': { target: 'gea::host::Display.invalidate' },
    'Display.getPixelFormat': { target: 'gea::host::Display.getPixelFormat' },
    'Display.setPixelFormat': { target: 'gea::host::Display.setPixelFormat', hints: ['string'] },
    'Display.getPanelPixelFormat': { target: 'gea::host::Display.getPanelPixelFormat' },
    'Display.getSupportedPixelFormats': { target: 'gea::host::Display.getSupportedPixelFormats' },
    'Display.setAA': { target: 'gea::host::Display.setAA', hints: ['number'] },
    'Display.setFlushConfig': { target: 'gea::host::Display.setFlushConfig' },
    'Display.epaperFullRefresh': { target: 'gea::host::Display.epaperFullRefresh' },
    'display.getBrightness': { target: 'gea::host::Display.getBrightness' },
    'display.setBrightness': { target: 'gea::host::Display.setBrightness', hints: ['number'] },
    'display.getDevicePixelRatio': { target: 'gea::host::Display.getDevicePixelRatio' },
    'display.setDevicePixelRatio': { target: 'gea::host::Display.setDevicePixelRatio', hints: ['number'] },
    'display.getFrameIntervalMs': { target: 'gea::host::Display.getFrameIntervalMs' },
    'display.setFrameIntervalMs': { target: 'gea::host::Display.setFrameIntervalMs', hints: ['number'] },
    'display.getFrameRate': { target: 'gea::host::Display.getFrameRate' },
    'display.setFrameRate': { target: 'gea::host::Display.setFrameRate', hints: ['number'] },
    'display.getOrientation': { target: 'gea::host::Display.getOrientation' },
    'display.setOrientation': { target: 'gea::host::Display.setOrientation', hints: ['string'] },
    'display.getSupportedOrientations': { target: 'gea::host::Display.getSupportedOrientations' },
    'display.setSupportedOrientations': { target: 'gea::host::Display.setSupportedOrientations' },
    'display.getAutoRotate': { target: 'gea::host::Display.getAutoRotate' },
    'display.setAutoRotate': { target: 'gea::host::Display.setAutoRotate', hints: ['boolean'] },
    'display.setVSync': { target: 'gea::host::Display.setVSync', hints: ['boolean'] },
    'display.setTextRasterCache': { target: 'gea::host::Display.setTextRasterCache', hints: ['boolean'] },
    'display.setTextSolidBackdrop': { target: 'gea::host::Display.setTextSolidBackdrop', hints: ['number'] },
    'display.invalidate': { target: 'gea::host::Display.invalidate' },
    'display.getPixelFormat': { target: 'gea::host::Display.getPixelFormat' },
    'display.setPixelFormat': { target: 'gea::host::Display.setPixelFormat', hints: ['string'] },
    'display.getPanelPixelFormat': { target: 'gea::host::Display.getPanelPixelFormat' },
    'display.getSupportedPixelFormats': { target: 'gea::host::Display.getSupportedPixelFormats' },
    'display.setAA': { target: 'gea::host::Display.setAA', hints: ['number'] },
    'display.setFlushConfig': { target: 'gea::host::Display.setFlushConfig' },
    'display.epaperFullRefresh': { target: 'gea::host::Display.epaperFullRefresh' },
    '__gea_Display.getBrightness': { target: 'gea::host::Display.getBrightness' },
    '__gea_Display.setBrightness': { target: 'gea::host::Display.setBrightness', hints: ['number'] },
    '__gea_Display.getDevicePixelRatio': { target: 'gea::host::Display.getDevicePixelRatio' },
    '__gea_Display.setDevicePixelRatio': { target: 'gea::host::Display.setDevicePixelRatio', hints: ['number'] },
    '__gea_Display.getFrameIntervalMs': { target: 'gea::host::Display.getFrameIntervalMs' },
    '__gea_Display.setFrameIntervalMs': { target: 'gea::host::Display.setFrameIntervalMs', hints: ['number'] },
    '__gea_Display.getFrameRate': { target: 'gea::host::Display.getFrameRate' },
    '__gea_Display.setFrameRate': { target: 'gea::host::Display.setFrameRate', hints: ['number'] },
    '__gea_Display.getOrientation': { target: 'gea::host::Display.getOrientation' },
    '__gea_Display.setOrientation': { target: 'gea::host::Display.setOrientation', hints: ['string'] },
    '__gea_Display.getSupportedOrientations': { target: 'gea::host::Display.getSupportedOrientations' },
    '__gea_Display.setSupportedOrientations': { target: 'gea::host::Display.setSupportedOrientations' },
    '__gea_Display.getAutoRotate': { target: 'gea::host::Display.getAutoRotate' },
    '__gea_Display.setAutoRotate': { target: 'gea::host::Display.setAutoRotate', hints: ['boolean'] },
    '__gea_Display.setVSync': { target: 'gea::host::Display.setVSync', hints: ['boolean'] },
    '__gea_Display.setTextRasterCache': { target: 'gea::host::Display.setTextRasterCache', hints: ['boolean'] },
    '__gea_Display.setTextSolidBackdrop': { target: 'gea::host::Display.setTextSolidBackdrop', hints: ['number'] },
    '__gea_Display.invalidate': { target: 'gea::host::Display.invalidate' },
    '__gea_Display.getPixelFormat': { target: 'gea::host::Display.getPixelFormat' },
    '__gea_Display.setPixelFormat': { target: 'gea::host::Display.setPixelFormat', hints: ['string'] },
    '__gea_Display.getPanelPixelFormat': { target: 'gea::host::Display.getPanelPixelFormat' },
    '__gea_Display.getSupportedPixelFormats': { target: 'gea::host::Display.getSupportedPixelFormats' },
    '__gea_Display.setAA': { target: 'gea::host::Display.setAA', hints: ['number'] },
    '__gea_Display.setFlushConfig': { target: 'gea::host::Display.setFlushConfig' },
    '__gea_Display.epaperFullRefresh': { target: 'gea::host::Display.epaperFullRefresh' },

    // ClockController — the framework's `Clock` import from `gea-embedded`.
    // `epochMs()` reads gettimeofday (wall clock; reflects GEADEV SETTIME),
    // distinct from Date.now() which is monotonic on ESP32.
    'Clock.epochMs': { target: 'gea::host::Clock.epochMs' },
    '__gea_Clock.epochMs': { target: 'gea::host::Clock.epochMs' },

    // ProfilerController — monotonic microsecond timing for app instrumentation.
    'Profiler.nowUs': { target: 'gea::host::Profiler.nowUs' },
    '__gea_Profiler.nowUs': { target: 'gea::host::Profiler.nowUs' },
    'Profiler.nowCycles': { target: 'gea::host::Profiler.nowCycles' },
    '__gea_Profiler.nowCycles': { target: 'gea::host::Profiler.nowCycles' },

    // Browser-compatible `localStorage` — persistent NVS-backed key/value for
    // TSX apps. A global (no import); mirrors the DOM Storage API.
    'localStorage.getItem': { target: 'gea::host::Storage.getItem', hints: ['string'] },
    'localStorage.setItem': { target: 'gea::host::Storage.setItem', hints: ['string', 'string'] },
    'localStorage.removeItem': { target: 'gea::host::Storage.removeItem', hints: ['string'] },
    'localStorage.clear': { target: 'gea::host::Storage.clear' },
    'localStorage.key': { target: 'gea::host::Storage.key', hints: ['number'] },

    // BatteryController — charge percentage from the platform PMU.
    'Battery.level': { target: 'gea::host::Battery.level' },
    '__gea_Battery.level': { target: 'gea::host::Battery.level' },

    // NotifyController — transient host->app notification channel.
    'Notify.text': { target: 'gea::host::Notify.text' },
    '__gea_Notify.text': { target: 'gea::host::Notify.text' },
    'Notify.seq': { target: 'gea::host::Notify.seq' },
    '__gea_Notify.seq': { target: 'gea::host::Notify.seq' },
    // DeviceControlController — host shell exec (macOS companion).
    'DeviceControl.exec': { target: 'gea::host::DeviceControl.exec', hints: ['string'] },
    '__gea_DeviceControl.exec': { target: 'gea::host::DeviceControl.exec', hints: ['string'] },

    'Camera.isAvailable': { target: 'gea::host::Camera.isAvailable' },
    'Camera.hasPermission': { target: 'gea::host::Camera.hasPermission' },
    'Camera.requestPermission': { target: 'gea::host::Camera.requestPermission' },
    'Camera.open': { target: 'gea::host::Camera.open', optionsRecord: true },
    'Camera.close': { target: 'gea::host::Camera.close' },
    'Camera.isOpen': { target: 'gea::host::Camera.isOpen' },
    'Camera.draw': { target: 'gea::host::Camera.draw', hints: ['number', 'number', 'number', 'number'] },
    'Camera.capture': { target: 'gea::host::Camera.capture', optionsRecord: true },
    'Camera.captureMirrored': { target: 'gea::host::Camera.captureMirrored' },
    'Camera.setFlash': { target: 'gea::host::Camera.setFlash', hints: ['string'] },
    'Camera.setZoom': { target: 'gea::host::Camera.setZoom', hints: ['number'] },
    'Camera.setMirror': { target: 'gea::host::Camera.setMirror', hints: ['boolean'] },
    // Options-record controls (mirror Camera.open): an object-literal argument
    // lowers to a TYPED anonymous struct (lowerAnonymousOptionsStruct) whose
    // fields the CameraFacade template overloads read directly via `requires`
    // member probes — no gea_cpp_value boxing. Non-literal args keep the boxed
    // record path, which the host readers still accept via record_get_literal.
    'Camera.setExposure': { target: 'gea::host::Camera.setExposure', optionsRecord: true },
    'Camera.setWhiteBalance': { target: 'gea::host::Camera.setWhiteBalance', optionsRecord: true },
    'Camera.setFocus': { target: 'gea::host::Camera.setFocus', optionsRecord: true },
    'Camera.deviceIdAt': { target: 'gea::host::Camera.deviceIdAt', hints: ['number'] },
    'Camera.deviceFacingAt': { target: 'gea::host::Camera.deviceFacingAt', hints: ['number'] },
  }
  const match = calls[path]
  if (!match) return null
  const lowered = args.map((arg, index) => {
    if (match.optionsRecord && arg.kind === 'object') {
      const typed = lowerAnonymousOptionsStruct(context, arg)
      if (typed) return typed
    }
    return lowerExpr(context, arg, match.hints?.[index] ?? 'any')
  })
  return lowered.every(Boolean) ? `${match.target}(${lowered.join(', ')})` : null
}

// An options-record literal for a host template overload (`Camera.setExposure(
// { mode, bias })`) constructs a TYPED anonymous struct carrying exactly the
// literal's fields, each typed by its lowered expression. The host reads the
// fields through `requires` member probes, so absent fields simply don't
// exist and the reader's fallback applies — no gea_cpp_value boxing, no
// dependency on a named `__gea_type_*` struct being emitted. Nested object
// fields (setFocus's `point: { x, y }`) recurse into nested anonymous
// structs. Bails (boxed fallback) when a field name isn't a clean C++
// identifier, since the host probes by member name.
function lowerAnonymousOptionsStruct(context: StoreMethodLowerContext, arg: GeaIrStoreExpr): string | null {
  if (arg.kind !== 'object' || arg.fields.length === 0) return null
  const locals: string[] = []
  const members: string[] = []
  const inits: string[] = []
  for (let index = 0; index < arg.fields.length; index += 1) {
    const field = arg.fields[index]
    if (sanitizeCppIdentifier(field.name) !== field.name) return null
    const value =
      field.value.kind === 'object'
        ? lowerAnonymousOptionsStruct(context, field.value)
        : lowerExpr(context, field.value, 'any')
    if (!value) return null
    locals.push(`auto __gea_opt_${index} = ${value};`)
    members.push(`decltype(__gea_opt_${index}) ${field.name};`)
    inits.push(`__gea_opt_${index}`)
  }
  return `([&]() { ${locals.join(' ')} struct { ${members.join(' ')} } __gea_options{${inits.join(', ')}}; return __gea_options; })()`
}

// An object literal pushed/unshifted/spliced into a TYPED-STORAGE store array
// constructs the element struct DIRECTLY. Without this, the literal lowers to
// a boxed gea_cpp_value record (shared entries vector allocation) that
// push_array_items immediately decodes back through
// `Element::__gea_from_value` — a build-then-unbuild round trip on every push.
//
// The storage may have been typed by EITHER side (the plugin's typedStorage
// metadata, or geatsc's checker via a restored JSDoc class-field hint that the
// plugin's shape analysis can't see — car-dashboard's `speedTicks:
// GaugeTick[] = []` has an empty-initializer shape). So the gate is COMPILE
// TIME: resolve the element type from the member itself and take the typed
// branch only when it's a real struct exposing `record_set_literal` (both
// geatsc's `__gea_type_X` interface structs — which also maintain their
// `__gea_has_*` presence flags through it — and the plugin's synthesized
// `<Store>_<field>_item` structs do). Untyped `vector<gea_cpp_value>` storage
// keeps the boxed record shape, bit-for-bit the same as lowerObject's.
function lowerTypedElementLiteral(context: StoreMethodLowerContext, fieldName: string, arg: GeaIrStoreExpr): string | null {
  if (arg.kind !== 'object' || arg.fields.length === 0) return null
  const member = `(*this).${sanitizeCppIdentifier(fieldName)}`
  const valueLocals: string[] = []
  const typedWrites: string[] = []
  const boxedWrites: string[] = []
  for (let index = 0; index < arg.fields.length; index += 1) {
    const literalField = arg.fields[index]
    const value = lowerExpr(context, literalField.value, 'any')
    if (!value) return null
    valueLocals.push(`auto __gea_value_${index} = ${value};`)
    // Typed element: write each field through the per-item-type `set_typed_field`
    // helper (gea_ir::set_typed_field), emitted for every typed store-array item
    // — synthesized AND interface-reuse, in BOTH lean and dynamic-fallback modes
    // (see generateTypedItemHelpers). It assigns the C++ field directly with no
    // gea_cpp_value box/unbox. The lean typed struct no longer carries
    // `record_set_literal` (generateTypedSlotMemberMethods strips it when the
    // dynamic fallback is off), so set_typed_field is the construction op that
    // works across modes.
    typedWrites.push(`gea_ir::set_typed_field(__gea_item, ${JSON.stringify(literalField.name)}, __gea_value_${index});`)
    boxedWrites.push(`__gea_record.record_set_literal(${JSON.stringify(literalField.name)}, gea_cpp_key(__gea_value_${index}));`)
  }
  // A C++20 templated lambda parameterised on the element type. `if constexpr`
  // only discards its untaken branch inside a template; in the non-template
  // store-method body a plain lambda would semantically check BOTH branches and
  // deduce `auto` from both — so a typed element (returns __GeaElem) and the
  // gea_cpp_value fallback (returns gea_cpp_value) would clash with "return type
  // must match previous return type". Templating on `__GeaElem` makes the
  // condition dependent, so exactly one branch is instantiated and type-deduced.
  return (
    `([&]<typename __GeaElem>() { ${valueLocals.join(' ')} ` +
    `if constexpr (std::is_same_v<__GeaElem, gea_cpp_value>) { ` +
    `gea_cpp_value __gea_record; __gea_record.kind = gea_cpp_value::kind_t::record; __gea_record.entries = std::make_shared<std::vector<std::pair<std::string, gea_cpp_value>>>(); ${boxedWrites.join(' ')} return __gea_record; ` +
    `} else { __GeaElem __gea_item{}; ${typedWrites.join(' ')} return __gea_item; } ` +
    `}.template operator()<typename std::decay_t<decltype(${member})>::value_type>())`
  )
}

function lowerElementLiteralForType(context: StoreMethodLowerContext, elementType: string, arg: GeaIrStoreExpr): string | null {
  if (arg.kind !== 'object' || arg.fields.length === 0) return null
  const writes: string[] = []
  for (const field of arg.fields) {
    const value = lowerExpr(context, field.value, 'any')
    if (!value) return null
    writes.push(`__gea_item.record_set_literal(${JSON.stringify(field.name)}, gea_cpp_key(${value}));`)
  }
  return `([&]() { ${elementType} __gea_item{}; ${writes.join(' ')} return __gea_item; })()`
}

function lowerObject(context: StoreMethodLowerContext, expr: Extract<GeaIrStoreExpr, { kind: 'object' }>): string | null {
  const writes = expr.fields.map((field) => {
    const value = lowerExpr(context, field.value, 'any')
    return value ? `__gea_record.record_set_literal(${JSON.stringify(field.name)}, gea_cpp_key(${value}));` : null
  })
  if (!writes.every(Boolean)) return null
  return `([&]() { gea_cpp_value __gea_record; __gea_record.kind = gea_cpp_value::kind_t::record; __gea_record.entries = std::make_shared<std::vector<std::pair<std::string, gea_cpp_value>>>(); ${writes.join(' ')} return __gea_record; })()`
}

function lowerArray(context: StoreMethodLowerContext, expr: Extract<GeaIrStoreExpr, { kind: 'array' }>): string | null {
  // An empty array literal must NOT be treated as vector<string> (every() is
  // vacuously true on []). It's typically `const items = []` accumulated via
  // push(record) then assigned to a record-typed store field, so the generic
  // gea_cpp_value vector is the only safe element type.
  if (expr.elements.length === 0) return 'std::vector<gea_cpp_value>{}'
  if (expr.elements.every((element) => element.kind === 'string')) {
    const elements = expr.elements.map((element) => lowerExpr(context, element, 'string'))
    return elements.every(Boolean) ? `std::vector<std::string>{${elements.join(', ')}}` : null
  }
  if (expr.elements.every((element) => element.kind === 'number')) {
    const elements = expr.elements.map((element) => lowerExpr(context, element, 'number'))
    return elements.every(Boolean) ? `std::vector<double>{${elements.join(', ')}}` : null
  }
  const elements = expr.elements.map((element) => {
    const lowered = lowerExpr(context, element, 'any')
    return lowered ? `gea_cpp_key(${lowered})` : null
  })
  return elements.every(Boolean) ? `std::vector<gea_cpp_value>{${elements.join(', ')}}` : null
}

function lowerBinary(context: StoreMethodLowerContext, expr: Extract<GeaIrStoreExpr, { kind: 'binary' | 'logical' }>, hint: StoreMethodHint): string | null {
  const op = normalizedBinaryOp(expr.op)
  // `+` is string concatenation ONLY when an operand is actually a string.
  // A purely numeric `+` whose RESULT is needed as a string (e.g. `${idx + 1}`
  // in a template literal, hint='string') must be evaluated as a numeric add
  // and THEN stringified — not split into `str(idx) + "1"`. Triggering concat
  // on `hint === 'string'` alone wrongly forced the numeric add down the
  // concat path (so `#${idx + 1}` rendered "#01","#11","#21"…). The numeric
  // path below + coerceForHint('string') already produces the correct
  // `gea_cpp_to_string((idx + 1.0))`, and `${5 + 3}` -> "8".
  if (op === '+' && (containsString(context, expr.left) || containsString(context, expr.right))) {
    const left = lowerExpr(context, expr.left, 'string')
    const right = lowerExpr(context, expr.right, 'string')
    return left && right ? `${left} + ${right}` : null
  }
  if (op === '|') {
    const left = lowerExpr(context, expr.left, 'number')
    const right = lowerExpr(context, expr.right, 'number')
    return left && right ? coerceForHint(`static_cast<double>(gea::runtime::numeric::to_int32(${left}) | gea::runtime::numeric::to_int32(${right}))`, hint) : null
  }
  if (op === '%') {
    const left = lowerExpr(context, expr.left, 'number')
    const right = lowerExpr(context, expr.right, 'number')
    // JS `%` is the floating-point remainder: the result takes the sign of the
    // dividend (`-7 % 16 === -7`). The previous lowering cast both operands to
    // std::size_t, but converting a negative double to an unsigned integer is
    // undefined behavior (yields 0 on 32-bit ARM), so any `%` with a negative
    // left operand silently produced 0 — e.g. a virtual-list's circular slot
    // recycling `(k - start) % SLOT_COUNT` collapsed every wrapped slot to the
    // same item. std::fmod matches JS semantics for all inputs.
    return left && right ? coerceForHint(`std::fmod(${left}, ${right})`, hint) : null
  }
  if (op === '<<') {
    const left = lowerExpr(context, expr.left, 'number')
    const right = lowerExpr(context, expr.right, 'number')
    return left && right ? coerceForHint(`static_cast<double>(gea::runtime::numeric::to_int32(${left}) << (gea::runtime::numeric::to_uint32(${right}) & 31u))`, hint) : null
  }
  if (op === '>>') {
    const left = lowerExpr(context, expr.left, 'number')
    const right = lowerExpr(context, expr.right, 'number')
    return left && right ? coerceForHint(`static_cast<double>(gea::runtime::numeric::to_int32(${left}) >> (gea::runtime::numeric::to_uint32(${right}) & 31u))`, hint) : null
  }
  if (op === '>>>') {
    const left = lowerExpr(context, expr.left, 'number')
    const right = lowerExpr(context, expr.right, 'number')
    return left && right ? coerceForHint(`static_cast<double>(gea::runtime::numeric::to_uint32(${left}) >> (gea::runtime::numeric::to_uint32(${right}) & 31u))`, hint) : null
  }
  const boolOp = op === '&&' || op === '||'
  // For equality / inequality / ordering, pick the operand hint from the
  // actual types — coercing a string param or string field to number would
  // emit `to_number(noteId) == "literal"` which fails to compile against a
  // std::string field. The previous logic hardcoded 'number', so equality
  // comparisons involving any string operand were silently miscompiled.
  const comparisonOp = op === '==' || op === '!=' || op === '<' || op === '>' || op === '<=' || op === '>='
  const stringComparison =
    comparisonOp && (containsString(context, expr.left) || containsString(context, expr.right))
  const operandHint: StoreMethodHint = boolOp ? 'boolean' : stringComparison ? 'string' : 'number'
  const left = lowerExpr(context, expr.left, operandHint)
  const right = lowerExpr(context, expr.right, operandHint)
  // A numeric/comparison result used in a string context (e.g. `${a * b}` in a
  // template literal) must be stringified; coerceForHint is a no-op for the
  // number/any hints so existing call sites are unaffected.
  if (!left || !right) return null
  const combined = `(${left} ${op} ${right})`
  // Comparison (`==`,`<`,…) and logical (`&&`,`||`) operators already produce a
  // C++ bool, so wrapping them in to_boolean() for a boolean hint is redundant
  // noise (it emitted `to_boolean((y >= 0.0))` in for-conditions and `&&`
  // operands). Coerce only when the boolean result feeds a non-boolean hint.
  if ((boolOp || comparisonOp) && hint === 'boolean') return combined
  return coerceForHint(combined, hint)
}

function lowerConditional(context: StoreMethodLowerContext, expr: Extract<GeaIrStoreExpr, { kind: 'conditional' }>, hint: StoreMethodHint): string | null {
  const test = lowerExpr(context, expr.test, 'boolean')
  const consequent = lowerExpr(context, expr.consequent, hint)
  const alternate = lowerExpr(context, expr.alternate, hint)
  return test && consequent && alternate ? `(${test} ? ${consequent} : ${alternate})` : null
}

function containsString(context: StoreMethodLowerContext, expr: GeaIrStoreExpr): boolean {
  if (expr.kind === 'string') return true
  if (expr.kind === 'identifier') {
    if (typeof context.constants.get(expr.name) === 'string') return true
    if (context.paramHints.get(expr.name) === 'string') return true
    return false
  }
  if (expr.kind === 'conditional') return containsString(context, expr.consequent) || containsString(context, expr.alternate)
  if (expr.kind === 'member') {
    const field = thisFieldName(expr)
    if (field && fieldHint(context.store, field) === 'string') return true
    const item = thisArrayItemFieldTarget(expr)
    if (item && arrayItemFieldHint(context.store, item.arrayField, item.itemField) === 'string') return true
    const aliasField = arrayElementAliasFieldTarget(context, expr)
    if (aliasField && arrayItemFieldHint(context.store, aliasField.alias.arrayField, aliasField.itemField) === 'string') return true
    return false
  }
  if (expr.kind === 'binary' && (expr.op === '+' || expr.op === '+=')) {
    return containsString(context, expr.left) || containsString(context, expr.right)
  }
  if (expr.kind === 'logical') return containsString(context, expr.left) || containsString(context, expr.right)
  return false
}

function normalizedBinaryOp(op: string): string {
  if (op === '===') return '=='
  if (op === '!==') return '!='
  return op
}

function constantExpr(value: number | string | boolean | null, hint: StoreMethodHint): string {
  // In a string context (e.g. string concatenation / template literals) every
  // constant must be stringified the way JS would — `5000 + ''` -> "5000",
  // `true + ''` -> "true", `null + ''` -> "null". Emitting the raw number
  // literal `5000.0` produces invalid `double + std::string` C++.
  if (hint === 'string') return `std::string(${JSON.stringify(String(value))})`
  if (typeof value === 'number') return cppNumber(value)
  if (typeof value === 'boolean') return value ? 'true' : 'false'
  if (value === null) return 'gea_cpp_value::null_v()'
  return hint === 'any' ? `std::string(${JSON.stringify(value)})` : JSON.stringify(value)
}

function coerceForHint(value: string, hint: StoreMethodHint): string {
  if (hint === 'string') return `gea_cpp_to_string(${value})`
  if (hint === 'boolean') return `gea::runtime::coerce::to_boolean(${value})`
  return value
}

function cppNumber(value: number): string {
  return Number.isInteger(value) ? `${value}.0` : String(value)
}

function parenthesize(value: string): string {
  return /^[A-Za-z0-9_:().]+$/.test(value) ? value : `(${value})`
}
