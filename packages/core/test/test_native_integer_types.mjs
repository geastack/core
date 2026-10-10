import assert from 'node:assert/strict'
import test from 'node:test'
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import ts from 'typescript'

const root = fileURLToPath(new URL('../', import.meta.url))
const manifest = JSON.parse(readFileSync(new URL('../package.json', import.meta.url), 'utf8'))
const declaration = resolve(root, manifest.exports['.'].types)
const runtime = resolve(root, manifest.exports['.'].default)
const entry = fileURLToPath(new URL('./native-integer-types.ts', import.meta.url))

const inspect = (specifier, native) => {
  const source = `import { Store } from '${specifier}';
    interface Ball { x: int32; y: int32; dx: int32; dy: int32 }
    export class BallStore extends Store {
      balls: Ball[] = [{ x: 0, y: 0, dx: 1, dy: -1 }];
    }
    export function widths(a: i32, b: int32, c: int, d: i64, e: int64): number[] {
      return [a, b, c, d, e];
    }`
  const options = {
    strict: true,
    noEmit: true,
    skipLibCheck: true,
    target: ts.ScriptTarget.ES2022,
    module: ts.ModuleKind.ESNext,
    moduleResolution: ts.ModuleResolutionKind.Bundler,
    lib: ['lib.es2022.d.ts'],
    types: [],
    ...(native ? { paths: { '@geastack/core': [runtime], 'gea-embedded': [runtime] } } : {})
  }
  const host = ts.createCompilerHost(options, true)
  const read = host.getSourceFile.bind(host)
  const exists = host.fileExists.bind(host)
  host.getSourceFile = (name, version, onError, fresh) =>
    resolve(name) === entry ? ts.createSourceFile(name, source, version, true, ts.ScriptKind.TS) : read(name, version, onError, fresh)
  host.fileExists = (name) => resolve(name) === entry || exists(name)
  const program = ts.createProgram({ rootNames: [entry], options, host })
  const file = program.getSourceFile(entry)
  assert.ok(file)
  assert.equal(
    ts.resolveModuleName(specifier, entry, options, host).resolvedModule?.resolvedFileName,
    native ? runtime : declaration
  )
  assert.ok(program.getSourceFile(declaration), 'integer aliases must come from the package declarations')
  const diagnostics = [
    ...program.getOptionsDiagnostics(),
    ...program.getGlobalDiagnostics(),
    ...program.getSyntacticDiagnostics(),
    ...program.getSemanticDiagnostics(file)
  ]
  assert.deepEqual(
    diagnostics.map((diagnostic) => `${diagnostic.code}: ${ts.flattenDiagnosticMessageText(diagnostic.messageText, '\n')}`),
    []
  )
  const checker = program.getTypeChecker()
  const ball = file.statements.find(ts.isInterfaceDeclaration)
  assert.ok(ball)
  for (const field of ball.members) assert.equal(checker.getTypeAtLocation(field).aliasSymbol?.name, 'int32')
  const widths = file.statements.find(ts.isFunctionDeclaration)
  assert.ok(widths)
  assert.deepEqual(
    widths.parameters.map((parameter) => checker.getTypeAtLocation(parameter).aliasSymbol?.name),
    ['i32', 'int32', 'int', 'i64', 'int64']
  )
  for (const parameter of widths.parameters)
    assert.ok(checker.isTypeAssignableTo(checker.getTypeAtLocation(parameter), checker.getNumberType()))
}

test('package exports expose global native integer aliases to an app without alias redeclarations', () => {
  inspect('@geastack/core', false)
})

test('native source aliases retain the same global integer declarations through runtime.ts', () => {
  for (const specifier of ['@geastack/core', 'gea-embedded']) inspect(specifier, true)
})
