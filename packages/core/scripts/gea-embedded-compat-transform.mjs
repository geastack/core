import fs from 'node:fs'
import path from 'node:path'
import { createRequire } from 'node:module'

const require = createRequire(new URL('../vendor/gea/package.json', import.meta.url))
const parser = require('@babel/parser')
const traverse = require('@babel/traverse').default
const generate = require('@babel/generator').default
const t = require('@babel/types')

const ignoredKeys = new Set([
  'loc',
  'start',
  'end',
  'leadingComments',
  'innerComments',
  'trailingComments',
  'extra',
])

function hasJsx(node) {
  if (!node || typeof node !== 'object') return false
  if (Array.isArray(node)) return node.some(hasJsx)
  if (typeof node.type === 'string' && node.type.startsWith('JSX')) return true
  for (const key of Object.keys(node)) {
    if (ignoredKeys.has(key)) continue
    if (hasJsx(node[key])) return true
  }
  return false
}

function isComponentName(name) {
  return /^[A-Z]/.test(name)
}

function isFunctionComponent(fn, name) {
  return !!fn && isComponentName(name) && hasJsx(fn.body)
}

function functionTemplateBody(fn) {
  if (t.isBlockStatement(fn.body)) {
    return fn.body.body.map((stmt) => t.cloneNode(stmt, true))
  }
  return [t.returnStatement(t.cloneNode(fn.body, true))]
}

function componentClassFromFunction(name, fn) {
  const params = fn.params.map((param) => t.cloneNode(param, true))
  const method = t.classMethod(
    'method',
    t.identifier('template'),
    params,
    t.blockStatement(functionTemplateBody(fn)),
  )
  if (fn.returnType) method.returnType = t.cloneNode(fn.returnType, true)
  const declaration = t.classDeclaration(t.identifier(name), t.identifier('Component'), t.classBody([method]))
  // A function component's props are its first parameter's type, and the class
  // this becomes has to say so: `Component`'s second type parameter is where a
  // component states them, and it defaults to `void`. Leaving it defaulted made
  // the generated `template({ label, value }: { ... })` an override whose
  // parameter is neither assignable to `void` nor assignable from it -- a
  // checker error on every function component that takes props, which is what
  // the settings/hid-clicker/app-launcher pipelines hit. The type is COPIED
  // from the parameter rather than inferred, so the two spellings cannot drift.
  const propsType = params[0]?.typeAnnotation
  if (propsType && t.isTSTypeAnnotation(propsType)) {
    declaration.superTypeParameters = t.tsTypeParameterInstantiation([
      t.tsTypeReference(t.identifier('GeaElement')),
      t.cloneNode(propsType.typeAnnotation, true),
    ])
  }
  return declaration
}

function componentClassFromVariableDeclarator(declarator) {
  if (!t.isIdentifier(declarator.id)) return null
  const name = declarator.id.name
  const init = declarator.init
  if (!isFunctionComponent(init, name)) return null
  return componentClassFromFunction(name, init)
}

// `GeaElement` is an interface, so it must be imported `type`-only: the staged
// tree is bundled as well as checked, and a value import of a name the runtime
// module does not export at runtime is an unresolved export, not a no-op.
function runtimeImportSpecifier(name) {
  const specifier = t.importSpecifier(t.identifier(name), t.identifier(name))
  if (name === 'GeaElement') specifier.importKind = 'type'
  return specifier
}

// The names a generated component class spells: `Component` always, and
// `GeaElement` only when one of them states props -- it is the first type
// argument you have to pass in order to reach the second. `runtime.ts`
// re-exports it (via `runtime-surface.ts`) for exactly this kind of reason, so
// it resolves from the same module `Component` does.
function ensureComponentImport(ast, alsoGeaElement) {
  const wanted = alsoGeaElement ? ['Component', 'GeaElement'] : ['Component']
  for (const node of ast.program.body) {
    if (!t.isImportDeclaration(node)) continue
    if (node.source.value !== 'gea-embedded' && node.source.value !== '@geajs/core') continue
    const present = new Set(
      node.specifiers.filter((spec) => t.isImportSpecifier(spec)).map((spec) => spec.imported.name),
    )
    for (const name of wanted) {
      if (!present.has(name)) node.specifiers.push(runtimeImportSpecifier(name))
    }
    return
  }

  ast.program.body.unshift(
    t.importDeclaration(wanted.map(runtimeImportSpecifier), t.stringLiteral('gea-embedded')),
  )
}

function runtimeCoreSpecifierName(spec) {
  if (!t.isImportSpecifier(spec)) return null
  const imported = spec.imported
  const name = t.isIdentifier(imported) ? imported.name : imported.value
  return name === 'Component' || name === 'Store' ? name : null
}

function importHasSpecifier(node, importedName, localName) {
  return node.specifiers.some((spec) => {
    if (!t.isImportSpecifier(spec)) return false
    const imported = spec.imported
    const name = t.isIdentifier(imported) ? imported.name : imported.value
    return name === importedName && spec.local.name === localName
  })
}

function ensureGeaEmbeddedRuntimeImport(ast, specs) {
  let target = ast.program.body.find(
    (node) => t.isImportDeclaration(node) && node.source.value === 'gea-embedded',
  )
  if (!target) {
    target = t.importDeclaration([], t.stringLiteral('gea-embedded'))
    ast.program.body.unshift(target)
  }
  for (const spec of specs) {
    const importedName = runtimeCoreSpecifierName(spec)
    if (!importedName) continue
    if (importHasSpecifier(target, importedName, spec.local.name)) continue
    target.specifiers.push(t.cloneNode(spec, true))
  }
}

function normalizeRuntimeCoreImports(ast) {
  let changed = false
  const nextBody = []
  const movedSpecs = []

  for (const node of ast.program.body) {
    if (!t.isImportDeclaration(node) || !['@geastack/core', '@geajs/core'].includes(node.source.value)) {
      nextBody.push(node)
      continue
    }

    const keep = []
    for (const spec of node.specifiers) {
      if (runtimeCoreSpecifierName(spec)) {
        movedSpecs.push(spec)
        changed = true
      } else {
        keep.push(spec)
      }
    }

    if (keep.length > 0) {
      node.specifiers = keep
      nextBody.push(node)
    } else {
      changed = true
    }
  }

  if (!changed) return false
  ast.program.body = nextBody
  ensureGeaEmbeddedRuntimeImport(ast, movedSpecs)
  return true
}

function transformTopLevelStatement(stmt) {
  if (t.isFunctionDeclaration(stmt) && stmt.id && isFunctionComponent(stmt, stmt.id.name)) {
    return { changed: true, nodes: [componentClassFromFunction(stmt.id.name, stmt)] }
  }

  if (t.isExportNamedDeclaration(stmt) && t.isFunctionDeclaration(stmt.declaration)) {
    const decl = stmt.declaration
    if (decl.id && isFunctionComponent(decl, decl.id.name)) {
      return {
        changed: true,
        nodes: [t.exportNamedDeclaration(componentClassFromFunction(decl.id.name, decl), [])],
      }
    }
  }

  if (t.isVariableDeclaration(stmt) && stmt.declarations.length === 1) {
    const classDecl = componentClassFromVariableDeclarator(stmt.declarations[0])
    if (classDecl) return { changed: true, nodes: [classDecl] }
  }

  if (
    t.isExportNamedDeclaration(stmt) &&
    t.isVariableDeclaration(stmt.declaration) &&
    stmt.declaration.declarations.length === 1
  ) {
    const classDecl = componentClassFromVariableDeclarator(stmt.declaration.declarations[0])
    if (classDecl) return { changed: true, nodes: [t.exportNamedDeclaration(classDecl, [])] }
  }

  return { changed: false, nodes: [stmt] }
}

export function transformGeaEmbeddedCompatSource(code, filename, onSourceMap) {
  if ((!code.includes('<') || !code.includes('>')) && !code.includes('@geastack/core') && !code.includes('@geajs/core')) return code

  const ast = parser.parse(code, {
    sourceType: 'module',
    plugins: ['jsx', 'typescript', 'classProperties', 'classPrivateProperties'],
    sourceFilename: filename,
  })

  let changed = false
  let transformedComponents = false
  const nextBody = []
  for (const stmt of ast.program.body) {
    const transformed = transformTopLevelStatement(stmt)
    if (transformed.changed) {
      changed = true
      transformedComponents = true
    }
    nextBody.push(...transformed.nodes)
  }
  ast.program.body = nextBody
  changed = normalizeRuntimeCoreImports(ast) || changed
  if (!changed) return code

  if (transformedComponents) {
    // `GeaElement` is needed only if one of the classes we just made states
    // props -- that is the only place the generated code spells it.
    const statesProps = ast.program.body.some((node) => {
      const declaration = t.isExportNamedDeclaration(node) ? node.declaration : node
      return t.isClassDeclaration(declaration) && declaration.superTypeParameters != null
    })
    ensureComponentImport(ast, statesProps)
  }

  const result = generate(ast, { retainLines: true, sourceMaps: !!onSourceMap, sourceFileName: filename }, code)
  if (onSourceMap) onSourceMap(result.map)
  return result.code
}

// ─── JSON-import constant folding ────────────────────────────────────────────
//
// `import data from './x.json'` followed by `data.notes` / `data.selectedId`
// reads. The geatsc C++ pipeline has no native lowering for a JSON module's
// default-export object: vite inlines it as a module-level object literal,
// which geatsc's global collector defers and BOXES into a `gea_cpp_value`
// global — every read goes through dynamic `record_get_literal`, and a store
// field seeded from it loses its array-of-objects shape (the vite-plugin-gea
// IR extractor sees a member expression, not a literal), so keyed lists
// silently render nothing.
//
// JSON modules are static data, so fold them at the source level instead:
// every member-access read of the imported binding is replaced with the
// actual JSON value as an inline literal, and the import is dropped once all
// references fold. Downstream (IR shape extraction, typed store-array
// storage, geatsc record lowering) then sees exactly what it would have seen
// had the data been written inline — the fully native path.
//
// Folding duplicates the value per reference site, which would split JS
// object identity if the same subtree were read through two aliases and
// mutated through one. To stay semantics-preserving we fold only when it is
// provably safe and otherwise leave the file untouched (boxed but correct):
//   - every reference to the binding must be a member-access read chain
//     (no bare uses, calls, exports, assignments, ++/--, delete, spread);
//   - per top-level JSON key, either all folded reads are scalars
//     (copies carry no identity) or there is exactly one read.

function jsonValueToAst(value) {
  if (value === null) return t.nullLiteral()
  switch (typeof value) {
    case 'string':
      return t.stringLiteral(value)
    case 'boolean':
      return t.booleanLiteral(value)
    case 'number':
      if (!Number.isFinite(value)) return null
      return value < 0 ? t.unaryExpression('-', t.numericLiteral(-value)) : t.numericLiteral(value)
    case 'object': {
      if (Array.isArray(value)) {
        const elements = value.map(jsonValueToAst)
        if (elements.some((element) => element === null)) return null
        return t.arrayExpression(elements)
      }
      const properties = []
      for (const [key, child] of Object.entries(value)) {
        const childAst = jsonValueToAst(child)
        if (childAst === null) return null
        const keyNode = /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(key) ? t.identifier(key) : t.stringLiteral(key)
        properties.push(t.objectProperty(keyNode, childAst))
      }
      return t.objectExpression(properties)
    }
    default:
      return null
  }
}

// Static member-access key for `obj.key` / `obj["key"]` / `obj[0]`, else null.
function memberAccessKey(member) {
  if (!member.computed && t.isIdentifier(member.property)) return member.property.name
  if (member.computed && t.isStringLiteral(member.property)) return member.property.value
  if (member.computed && t.isNumericLiteral(member.property)) return member.property.value
  return null
}

// Walk up the longest static member-access chain rooted at `refPath` and
// return { topPath, keys } — the outermost member-expression path plus the
// JSON key path it selects. Returns null when the chain breaks (computed
// non-literal key) or the chain is used as a write target.
function memberChainFromReference(refPath) {
  let top = refPath
  const keys = []
  while (t.isMemberExpression(top.parent) && top.parent.object === top.node) {
    const key = memberAccessKey(top.parent)
    if (key === null) return null
    keys.push(key)
    top = top.parentPath
  }
  if (keys.length === 0) return null
  const parent = top.parent
  if (t.isAssignmentExpression(parent) && parent.left === top.node) return null
  if (t.isUpdateExpression(parent)) return null
  if (t.isUnaryExpression(parent) && parent.operator === 'delete') return null
  return { topPath: top, keys }
}

function jsonValueAtPath(root, keys) {
  let current = root
  for (const key of keys) {
    if (current === null || typeof current !== 'object') return undefined
    current = current[key]
  }
  return current
}

const isScalarJsonValue = (value) => value === null || typeof value !== 'object'

export function inlineJsonImports(code, filename) {
  if (!code.includes('.json')) return code

  let ast
  try {
    ast = parser.parse(code, {
      sourceType: 'module',
      plugins: ['jsx', 'typescript', 'classProperties', 'classPrivateProperties'],
      sourceFilename: filename,
    })
  } catch {
    return code
  }

  let changed = false
  traverse(ast, {
    ImportDeclaration(importPath) {
      const source = importPath.node.source.value
      if (!source.endsWith('.json') || !source.startsWith('.')) return
      // Only the default-import form (TS `resolveJsonModule`'s canonical
      // shape). Named/namespace specifiers on a JSON import are left alone.
      if (importPath.node.specifiers.length !== 1 || !t.isImportDefaultSpecifier(importPath.node.specifiers[0])) return
      const localName = importPath.node.specifiers[0].local.name
      const jsonFile = path.resolve(path.dirname(filename), source)
      let data
      try {
        data = JSON.parse(fs.readFileSync(jsonFile, 'utf8'))
      } catch {
        return
      }
      const binding = importPath.scope.getBinding(localName)
      if (!binding || binding.constantViolations.length > 0) return

      const folds = []
      const readsByTopLevelKey = new Map()
      for (const refPath of binding.referencePaths) {
        const chain = memberChainFromReference(refPath)
        if (!chain) {
          warnSkippedJsonFold(filename, source, localName, 'it is used outside a static property read')
          return
        }
        const value = jsonValueAtPath(data, chain.keys)
        if (value === undefined) {
          warnSkippedJsonFold(filename, source, localName, `'${chain.keys.join('.')}' does not exist in the JSON`)
          return
        }
        const literal = jsonValueToAst(value)
        if (literal === null) {
          warnSkippedJsonFold(filename, source, localName, `'${chain.keys.join('.')}' has a non-JSON value`)
          return
        }
        const group = readsByTopLevelKey.get(chain.keys[0]) ?? []
        group.push(value)
        readsByTopLevelKey.set(chain.keys[0], group)
        folds.push({ topPath: chain.topPath, literal })
      }
      for (const [key, reads] of readsByTopLevelKey) {
        if (reads.length > 1 && reads.some((value) => !isScalarJsonValue(value))) {
          warnSkippedJsonFold(filename, source, localName, `'${key}' is read more than once and holds an object/array (folding would split its identity)`)
          return
        }
      }

      for (const fold of folds) fold.topPath.replaceWith(fold.literal)
      importPath.remove()
      changed = true
    },
  })
  if (!changed) return code

  return generate(ast, { retainLines: true }, code).code
}

function warnSkippedJsonFold(filename, source, localName, reason) {
  process.stderr.write(
    `${filename}: warning: JSON import '${localName}' from '${source}' was not inlined because ${reason}. ` +
      `On the embedded target the JSON object stays a dynamic boxed value (slow reads, and store list fields seeded from it do not render).\n`,
  )
}
