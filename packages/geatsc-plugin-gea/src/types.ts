export interface PluginOptionMap {
  [key: string]: string;
}

export interface EmitModule {
  sourceFile: { fileName: string };
  relativePath: string;
}

export interface PluginCppContext {
  options: PluginOptionMap;
  modules: EmitModule[];
}

export interface HostShimDefinitions {
  /** Exact host-owned declarations for the compiler's CommonJS module records. */
  commonJsGlobals?: Record<
    string,
    {
      global: "require" | "exports" | "module";
      declarationName: string;
      declarationFileName: string;
    }
  >;
  nativeTypes?: Record<string, string>;
  /** Identity-preserving views: destination carrier -> source carrier -> {value} template. */
  nativeViews?: Record<string, Record<string, string>>;
  /**
   * `new <AmbientConstructor>(...)` construct spellings, for a declared
   * ambient constructor interface whose instance type already has a
   * `nativeTypes` carrier (`Audio: AudioConstructor` -> `HTMLAudioElement` ->
   * `gea::host::HTMLAudioElement`). Keyed by the CONSTRUCTOR interface's own
   * declared name (`AudioConstructor`, not `Audio` or `HTMLAudioElement`) --
   * the same name a `declare var Audio: AudioConstructor` binds as its own
   * host protocol, one level up from the instance type `nativeTypes` already
   * carries. The template uses the `{arg0}`, `{arg1}`, ... convention shared
   * with `nativeMemberMethods` (never `embeddedHostClasses`' `{args}`, which
   * is a different, v1-only mechanism): a compiler that reads this table
   * derives each row's arity by counting the highest `{argN}` slot the
   * template names, so a zero-argument constructor (`MediaStream`) states a
   * template with no `{arg}` slot at all rather than a separate arity field.
   */
  nativeConstructors?: Record<string, string>;
  hostGlobalObjects?: Record<string, string>;
  /**
   * The C++ TYPE of each `hostGlobalObjects` value, keyed by the same global
   * name -- `window` -> `gea::host::WindowFacade` for the
   * `inline constexpr WindowFacade window{}` that `hostGlobalObjects.window`
   * names.
   *
   * `hostGlobalObjects` states the expression a path RESOLVES to; this states
   * what that expression IS. A compiler that only has the former can emit
   * `gea::host::window.innerWidth()` but cannot say what carries `window`
   * itself, so it falls back to expanding the ambient `Window & typeof
   * globalThis` declaration into a struct of its own -- a 26-field flattening
   * of lib.dom naming protocol tags no host declares, which is dead weight at
   * best and uncompilable at worst.
   *
   * Every row is the type of the object the sibling row names, read off the
   * host's own header, never a spelling invented by a compiler:
   * `packages/host/include/host/window.h`, `.../navigator.h`, `.../storage.h`.
   */
  hostGlobalObjectTypes?: Record<string, string>;
  hostNamespaces?: Record<string, string>;
  hostNamespaceIdentities?: Record<string, DeclaredFrameworkFunctionCoordinate>;
  hostNamespaceMethods?: Record<string, Record<string, string>>;
  // Like hostNamespaceMethods, but the binding also carries the C++ return type
  // so the call-site storage type is known (needed when the bundled source erased
  // the declared return type — e.g. `image.make(id): GeaEmbeddedImage`).
  nativeNamespaceMethods?: Record<
    string,
    Record<string, NativeNamespaceMethodBinding>
  >;
  hostNamespaceProperties?: Record<string, Record<string, string>>;
  hostNamespacePropertyAccessors?: Record<string, string[]>;
  hostNamespacePropertySetters?: Record<string, Record<string, string>>;
  documentMethods?: Record<string, string>;
  nativeDocumentMethods?: Record<string, NativeNamespaceMethodBinding>;
  /**
   * Intrinsic JSX tags whose SINGLE-TEXT-RUN form is a text node rather than a
   * view container.
   *
   * `<span>front</span>` on this host is one node -- `Document::createText()`
   * carrying the span's own class and style -- not a view with a text child.
   * The rule is v1's, and this list is the half of it that is a gea fact:
   * `isTextNodeTag` in `cpp-template-renderer.ts`, read by
   * `canUseTextNodeForElement` in the same file. The other half -- no element
   * children, exactly one text run -- is a DOM fact about the element's
   * contents and is stated by the compiler, not here.
   *
   * A tag absent from this list stays a container, which is the fail-closed
   * answer: a wrongly flattened element is a silently wrong tree, while a
   * container is merely one level deeper than v1's.
   */
  elementTextLeafTags?: string[];
  domElementMethods?: Record<string, string>;
  domTokenListMethods?: Record<string, string>;
  domElementPropertyGetters?: Record<string, string>;
  domElementPropertySetters?: Record<string, string>;
  nativeMemberMethods?: Record<string, NativeMemberBinding[]>;
  nativeMemberPropertyGetters?: Record<string, NativeMemberBinding[]>;
  nativeMemberPropertySetters?: Record<string, NativeMemberBinding[]>;
  canvasContextMethods?: Record<string, string>;
  canvasContextNoThrowMethods?: string[];
  canvasContextPropertySetters?: Record<string, string>;
  embeddedHostConstants?: Record<string, { emit: string; type: string }>;
  embeddedHostFunctions?: Record<string, string>;
  /** Headers inspected for native signatures; no duplicated scalar/callback type metadata. */
  nativeFunctionSignatureSources?: Record<string, { packageName: string; header: string }>;
  embeddedHostNoThrowFunctions?: string[];
  hostNamespaceNoThrowMethods?: Record<string, string[]>;
  embeddedHostFunctionReturnTypes?: Record<string, string>;
  embeddedHostClasses?: Record<string, EmbeddedHostClassBinding>;
  hostExternDeclarations?: Record<string, string[]>;
  /** Versioned framework-specific semantic protocols. Generic builtins never belong here. */
  frameworkProtocols?: readonly FrameworkProtocol[];
  /**
   * Plugin-declared, UNVERIFIED closed-world interface-dispatch facts:
   * `interfaceName` (a local structural interface with no `implements` link
   * to any class) has EXACTLY the listed classes as its only runtime
   * implementers in gea. The compiler does not re-derive or verify this.
   */
  declaredClosedWorldInterfaceDispatch?: DeclaredClosedWorldInterfaceDispatch[];
  nullishPropertyFallbackProtocols?: NullishPropertyFallbackProtocol[];
  classFactoryHooks?: ClassFactoryHook[];
  // Opt in to embedded symbol lowering: a `unique symbol` const used as a
  // property key lowers to its named string key (`__sym_<name>`) instead of a
  // boxed `gea_cpp_value` symbol. The general TypeScript/test262 path leaves it
  // off so dynamic Symbol identity/registry semantics are preserved.
  lowerSymbolsAsNamedMembers?: boolean;
}

export interface EmbeddedHostClassBinding {
  factory?: string;
  wrapper: string;
  allowConcrete?: boolean;
  construct?: string;
}

export interface NativeMemberBinding {
  extern?: string;
  receiverTypes?: string[];
  emit?: string;
  returnType?: string;
  noThrow?: boolean;
  argumentRoles?: readonly HostRuntimeCallableArgumentRole[];
  dynamicRecordSchemas?: readonly HostRuntimeDynamicRecordSchema[];
}

export interface NativeNamespaceMethodBinding {
  emit: string;
  returnType?: string;
  noThrow?: boolean;
  argumentRoles?: readonly HostRuntimeCallableArgumentRole[];
  dynamicRecordSchemas?: readonly HostRuntimeDynamicRecordSchema[];
}

export type HostRuntimeValueRole = "checker-carrier" | "dynamic-value";

export interface HostRuntimeDynamicRecordFieldRole {
  readonly key: string;
  readonly value: "checker-carrier";
}

export interface HostRuntimeDynamicRecordParameterRole {
  readonly kind: "dynamic-record";
  readonly protocolId: string;
}

export interface HostRuntimeDynamicRecordSchema {
  readonly protocolId: string;
  readonly fields: readonly HostRuntimeDynamicRecordFieldRole[];
}

export type HostRuntimeCallbackParameterRole =
  HostRuntimeValueRole | HostRuntimeDynamicRecordParameterRole;

export interface HostRuntimeCallbackArgumentRole {
  readonly kind: "callback";
  readonly parameters: readonly HostRuntimeCallbackParameterRole[];
  readonly result: HostRuntimeValueRole | "void";
  readonly lifetime: "call" | "retained";
}

export type HostRuntimeCallableArgumentRole =
  HostRuntimeValueRole | HostRuntimeCallbackArgumentRole;

export interface TrackedProxyProtocolRoles {
  readonly helperFunctions: Readonly<{
    plain: string;
    hidden: string;
    raw: string;
    queue: string;
    wrap: string;
    readTracker: string;
  }>;
  readonly queueOperations: Readonly<{
    hasQueuedConsumers: string;
    flush: string;
    fireBucket: string;
    observe: string;
    observeDirect: string;
  }>;
  readonly stateGlobals: Readonly<{
    activeTracker: string;
    privateState: string;
  }>;
  readonly stateSlots: Readonly<{
    pending: string;
    scheduled: string;
    observers: string;
    rootObservers: string;
    derived: string;
    proxy: string;
    direct: string;
  }>;
  readonly changeEnvelope: Readonly<{
    property: string;
    pathParts: string;
    type: string;
    target: string;
    updateValue: string;
  }>;
  readonly stringKeyTraps: Readonly<{
    arrayPush: string;
    nestedCacheSentinel: string;
  }>;
}

export interface TrackedProxyProtocolPayload {
  recordLayoutContractId: string;
  baseClassNames: string[];
  roles: TrackedProxyProtocolRoles;
  proxyFactoryFunction?: DeclaredFrameworkFunctionCoordinate;
  readTrackerFunction?: DeclaredFrameworkFunctionCoordinate;
  observeFunction?: DeclaredFrameworkFunctionCoordinate;
  observeDirectFunction?: DeclaredFrameworkFunctionCoordinate;
  dirtyPropsSymbol?: DeclaredFrameworkFunctionCoordinate;
  dirtySymbol?: DeclaredFrameworkFunctionCoordinate;
  rawSymbol?: DeclaredFrameworkFunctionCoordinate;
  /** Exact exported JS fallback whose embedded implementation is the configured native predicate. */
  plainValueFunction?: DeclaredFrameworkFunctionCoordinate;
  /** Versioned semantic domain shared by the JS fallback and embedded helper. */
  plainValueSemantics?: "tracked-proxy-plain-value-v1";
  /** C++ helper accepting the protocol value carrier and returning native bool. */
  plainValueHelper?: string;
  readTrackerIfActiveHelper?: string;
  fieldSetHelper?: string;
  typedFieldSetHelper?: string;
  vectorItemPropertySetHelper?: string;
  arrayItemChangeFactory?: string;
  methodBatchGuard?: string;
}

export interface TrackedProxyFrameworkProtocol {
  readonly kind: "tracked-proxy-v1";
  readonly semanticCategory: "framework-protocol";
  readonly id: string;
  readonly version: 1;
  readonly payload: TrackedProxyProtocolPayload;
}

export interface RuntimeRecordLayoutFrameworkProtocol {
  readonly kind: "runtime-record-layout-v1";
  readonly semanticCategory: "framework-protocol";
  readonly id: string;
  readonly version: 1;
  readonly payload: RuntimeRecordProtocolPayload;
}

export interface HostNativeCallableSidecarFrameworkProtocol {
  readonly kind: "host-native-callable-sidecar-v1";
  readonly semanticCategory: "framework-protocol";
  readonly id: string;
  readonly version: 1;
  readonly payload: {
    readonly callable: DeclaredFrameworkFunctionCoordinate;
    readonly operations: {
      readonly disposerContainedCallbacks?: Readonly<{
        readonly parameterIndices: readonly number[];
      }>;
      readonly retainedCleanupLifecycle?: Readonly<{
        readonly disposerParameterIndex: number;
        readonly registrationTarget: Readonly<{
          readonly owner: DeclaredFrameworkFunctionCoordinate;
          readonly instanceMethod: string;
        }>;
        readonly cleanupParameterIndex: number;
      }>;
      readonly callableExpandoProperties?: readonly Readonly<{
        readonly key: string;
        readonly receiverTypes: readonly string[];
        readonly receiverRole: "host-native-handle";
        readonly valueRole: "callable";
        readonly parameterTransports?: readonly (
          "checker-carrier" | HostRuntimeDynamicRecordParameterRole
        )[];
      }>[];
      readonly dataExpandoProperties?: readonly Readonly<{
        readonly key: string;
        readonly receiverTypes: readonly string[];
        readonly receiverRole: "host-native-handle";
        readonly valueRole: "dynamic";
      }>[];
      readonly structuralDynamicParameters?: readonly Readonly<{
        readonly parameterIndex: number;
        readonly structuralType: "object";
        readonly transport: "declared-dynamic";
      }>[];
    };
  };
}

export type FrameworkProtocol =
  | TrackedProxyFrameworkProtocol
  | RuntimeRecordLayoutFrameworkProtocol
  | HostNativeCallableSidecarFrameworkProtocol;

export interface DeclaredFrameworkFunctionCoordinate {
  moduleSpecifier: string;
  exportName: string;
}

export interface DeclaredClosedWorldInterfaceDispatch {
  interfaceName: string;
  implementingClasses: DeclaredFrameworkFunctionCoordinate[];
}

export interface RuntimeRecordProtocolPayload {
  keyedEntry?: RuntimeKeyedEntryRecordProtocol;
  change?: RuntimeChangeRecordProtocol;
  arrayItemChange?: RuntimeArrayItemChangeRecordProtocol;
  listChange?: RuntimeListChangeRecordProtocol;
}

export interface RuntimeKeyedEntryRecordProtocol {
  fields: {
    key: string;
    item: string;
    element: string;
    disposer: string;
    observer: string;
  };
  factory: string;
}

export interface RuntimeChangeRecordProtocol {
  previousValueField: string;
  newValueField: string;
  factory: string;
}

export interface RuntimeArrayItemChangeRecordProtocol {
  markerField: string;
  markerValue?: boolean;
  indexField: string;
  previousValueField: string;
  newValueField: string;
  itemDirtyField?: string;
  factory: string;
  runtimeKind?: string;
}

export interface RuntimeListChangeRecordProtocol {
  typeField: string;
  startField: string;
  countField: string;
  appendType: string;
  removeType: string;
  reorderType: string;
  updateType: string;
  factory: string;
}

export interface NullishPropertyFallbackProtocol {
  propertyName: string;
  helper: string;
}

export interface ClassFactoryHook {
  baseClassName: string;
  returnedValueStatements?: string[];
  defaultValueStatements?: string[];
}

export interface HostBindingAnalysisPatch {
  bindings?: string[];
  features?: string[];
}

export interface PluginDiagnostic {
  kind: "unsupported";
  code: string;
  message: string;
  file?: string;
  line?: number;
  column?: number;
  // Absent or 'error' aborts emit. 'warning' is reported but does not block
  // compilation (matches the core GeatscDiagnostic severity contract).
  severity?: "error" | "warning";
}

export interface GeatscPlugin {
  name: string;
  configure?(context: { entry: string; options: PluginOptionMap }): {
    allowAny?: boolean;
    hostShims?: HostShimDefinitions;
  };
  validate?(): PluginDiagnostic[];
  analyzeHostBindings?(context: {
    entry: string;
    options: PluginOptionMap;
  }): HostBindingAnalysisPatch;
  createCppBackend?(): {
    transformModules?(
      modules: EmitModule[],
      context: PluginCppContext,
    ): EmitModule[];
    transformGeneratedSources?(
      sources: Array<{ fileName: string; source: string }>,
      context: PluginCppContext,
    ): Array<{ fileName: string; source: string }>;
  };
}

export interface GeaIrBundleV1 {
  schema: "gea-ir";
  version: 1;
  entry: string;
  modules: GeaIrModule[];
  components: GeaIrComponent[];
  stores: GeaIrStore[];
  hostCapabilities: string[];
}

export interface GeaIrModule {
  id: string;
  file: string;
  components: string[];
  stores: string[];
}

export interface GeaIrComponent {
  id: string;
  module: string;
  exportName: string;
  runtimeBase: GeaIrRuntimeBase;
  template: GeaIrTemplate;
  sourceSpan?: GeaIrSourceSpan;
  // Present when the component extends `ReactiveComponent` — it holds its own
  // reactive state and is compiled as a lean component-as-store (the component
  // instance IS its backing store; `this.<field>`/`this.<getter>` bindings and
  // mutating methods wire through the mounted renderer). Absent for plain
  // stateless `Component` subclasses (no reactive overhead).
  reactiveState?: GeaIrComponentReactiveState;
}

export interface GeaIrComponentReactiveState {
  fields: GeaIrStoreField[];
  methods?: GeaIrStoreMethod[];
  getters?: GeaIrStoreGetter[];
  constants?: GeaIrConstant[];
}

export type GeaIrRuntimeBase =
  | "static"
  | "static-element"
  | "compiled"
  | "tiny-reactive"
  | "lean-reactive"
  | "reactive";

export interface GeaIrTemplate {
  html: string;
  slots: GeaIrSlot[];
}

export interface GeaIrSlot {
  index: number;
  kind: string;
  walk: number[];
  walkKinds?: Array<{ elem: number } | { child: number }>;
  expr?: string;
  exprPath?: string[];
  exprObjectFields?: GeaIrExpressionObjectField[];
  payload?: unknown;
  directText?: boolean;
}

export interface GeaIrExpressionObjectField {
  name: string;
  expr: string;
  exprPath?: string[];
}

export interface GeaIrKeyedListPayload {
  mapCallback?: unknown;
  itemParam?: string;
  indexParam?: string;
  rowTemplate?: GeaIrTemplate;
  [key: string]: unknown;
}

export interface GeaIrStore {
  id: string;
  module: string;
  className: string;
  runtimeBase: "compiled" | "lean";
  fields: GeaIrStoreField[];
  methods?: GeaIrStoreMethod[];
  getters?: GeaIrStoreGetter[];
  constants?: GeaIrConstant[];
  sourceSpan?: GeaIrSourceSpan;
  // Synthesized from a ReactiveComponent's own reactive state (component-as-store).
  // The class IS the geatsc-compiled typed component: its method bodies are already
  // correct (Signal writes notify), so store-method re-lowering must SKIP it, and
  // its fields read through typed `store->field.get()` reader overloads.
  selfStore?: boolean;
}

// A `get x()` accessor on a Store (see the producer's GeaIrStoreGetter). Used by
// the embedded target to back a reactive `{this.x.map(...)}` list with a derived
// array: `deps` are the reactive fields that recompute the list, and
// `elementTypeName`/`shape` describe the row element type.
export interface GeaIrStoreGetter {
  name: string;
  returnsArray: boolean;
  elementTypeName?: string;
  shape?: GeaIrStoreValueShape;
  deps: string[];
  body: string;
  ops?: GeaIrStoreStmt[];
  sourceSpan?: GeaIrSourceSpan;
}

export interface GeaIrStoreField {
  name: string;
  initializer?: string;
  shape?: GeaIrStoreValueShape;
  // When set, the field is stored as `std::vector<itemType>` (typed
  // C++ struct elements) instead of the dynamic
  // `std::vector<gea_cpp_value>`. Computed in one place at IR-build time
  // (see `attachTypedStorage` in cpp-stores.ts) so every consumer
  // (`applyTypedArrayStorage`, `lowerStoreMethod`, the stores generator)
  // shares the exact same view of which fields are typed and what their
  // helper names are.
  typedStorage?: GeaIrStoreFieldTypedStorage;
}

export interface GeaIrStoreFieldTypedStorage {
  // The resolved C++ element symbol. For the interface-reuse case this is the
  // geatsc-declared struct name `__gea_type_<Name>` stored WITHOUT the leading
  // `::` (the `::` qualifier is added at type-use sites). For the synthesized
  // case it stays `<Store>_<field>_item`.
  itemType: string;
  readerName: string;
  // When true, the array element type reuses geatsc's declared interface
  // struct `::__gea_type_<Name>` (global scope) instead of a synthesized
  // `<Store>_<field>_item` (in `namespace gea_ir`). Opt-in: only set when the
  // store field is annotated with a named interface whose fields are all
  // primitive.
  useInterfaceStruct?: boolean;
  // The original TS interface name (e.g. `CityState`), retained so downstream
  // passes can re-derive `__gea_type_<Name>` / force-emit hints.
  elementTypeName?: string;
}

export type GeaIrStoreValueShape =
  | { kind: "array"; element?: GeaIrStoreValueShape; elementTypeName?: string }
  | { kind: "object"; fields: GeaIrStoreField[] }
  | { kind: "literal"; valueType: "string" | "number" | "boolean" | "null" };

export interface GeaIrStoreMethod {
  name: string;
  params: GeaIrStoreMethodParam[];
  body: string;
  ops?: GeaIrStoreStmt[];
  sourceSpan?: GeaIrSourceSpan;
}

export interface GeaIrStoreMethodParam {
  name: string;
  valueType?: "string" | "number" | "boolean";
}

export type GeaIrConstantPrimitiveType =
  "string" | "number" | "boolean" | "null";

export interface GeaIrConstantObjectField {
  name: string;
  value: string;
  valueType: GeaIrConstantPrimitiveType;
}

export interface GeaIrConstantObjectArrayItem {
  fields: GeaIrConstantObjectField[];
}

export interface GeaIrConstant {
  name: string;
  value: string;
  valueType: GeaIrConstantPrimitiveType | "object-array";
  items?: GeaIrConstantObjectArrayItem[];
}

export type GeaIrStoreStmt =
  | {
      kind: "var";
      name: string;
      mutable?: boolean;
      init?: GeaIrStoreExpr;
      localType?: GeaIrStoreLocalType;
    }
  | { kind: "assign"; target: GeaIrStoreExpr; value: GeaIrStoreExpr }
  | { kind: "expr"; expr: GeaIrStoreExpr }
  | {
      kind: "if";
      test: GeaIrStoreExpr;
      consequent: GeaIrStoreStmt[];
      alternate?: GeaIrStoreStmt[];
    }
  | {
      kind: "for";
      init?: GeaIrStoreStmt;
      test?: GeaIrStoreExpr;
      update?: GeaIrStoreExpr;
      body: GeaIrStoreStmt[];
    }
  | { kind: "return"; value?: GeaIrStoreExpr };

export type GeaIrStoreLocalType =
  | { kind: "array"; elementTypeName: string }
  | { kind: "array"; elementPrimitive: "number" | "string" | "boolean" };

export type GeaIrStoreExpr =
  | { kind: "identifier"; name: string }
  | { kind: "this" }
  | { kind: "number"; value: number }
  | { kind: "string"; value: string }
  | { kind: "boolean"; value: boolean }
  | { kind: "null" }
  | {
      kind: "member";
      object: GeaIrStoreExpr;
      property: string;
      computed?: false;
    }
  | { kind: "index"; object: GeaIrStoreExpr; index: GeaIrStoreExpr }
  | { kind: "call"; callee: GeaIrStoreExpr; args: GeaIrStoreExpr[] }
  | { kind: "object"; fields: Array<{ name: string; value: GeaIrStoreExpr }> }
  | { kind: "array"; elements: GeaIrStoreExpr[] }
  | { kind: "unary"; op: string; arg: GeaIrStoreExpr }
  | { kind: "binary"; op: string; left: GeaIrStoreExpr; right: GeaIrStoreExpr }
  | { kind: "logical"; op: string; left: GeaIrStoreExpr; right: GeaIrStoreExpr }
  | {
      kind: "conditional";
      test: GeaIrStoreExpr;
      consequent: GeaIrStoreExpr;
      alternate: GeaIrStoreExpr;
    }
  | { kind: "update"; op: string; arg: GeaIrStoreExpr; prefix: boolean };

export interface GeaIrSourceSpan {
  start?: number;
  end?: number;
}
