import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'
import vm from 'node:vm'
import ts from 'typescript'

const source = readFileSync(new URL('../runtime/host.ts', import.meta.url), 'utf8')
const tree = ts.createSourceFile('host.ts', source, ts.ScriptTarget.Latest, true)
const declarations = new Map()
for (const statement of tree.statements) {
  if (!ts.isVariableStatement(statement)) continue
  for (const declaration of statement.declarationList.declarations) {
    if (ts.isIdentifier(declaration.name)) declarations.set(declaration.name.text, statement.getText(tree))
  }
}
const profilerSource = [declarations.get('__gea_Profiler'), declarations.get('Profiler')].join('\n')

function loadProfiler(native) {
  const sandbox = { exports: {}, Date: { now: () => 123456 } }
  if (native) sandbox.__gea_Profiler = native
  vm.runInNewContext(ts.transpileModule(profilerSource, {
    compilerOptions: { module: ts.ModuleKind.CommonJS },
  }).outputText, sandbox)
  return sandbox.exports.Profiler
}

test('Profiler concrete runtime delegates both native clocks', () => {
  const profiler = loadProfiler({ nowUs: () => 345, nowCycles: () => 678 })
  assert.equal(profiler.nowUs(), 345)
  assert.equal(profiler.nowCycles(), 678)
})

test('Profiler fallback preserves desktop nanosecond units', () => {
  const profiler = loadProfiler()
  assert.equal(profiler.nowUs(), 123456000)
  assert.equal(profiler.nowCycles(), 123456000000)
})

test('Profiler concrete wrapper and ambient native declaration both typecheck', () => {
  const filename = '/profiler-runtime.ts'
  const text = profilerSource + '\nconst cycles: number = Profiler.nowCycles()\n'
  const options = { noEmit: true, strict: true, target: ts.ScriptTarget.ES2022 }
  const host = ts.createCompilerHost(options)
  const originalGetSourceFile = host.getSourceFile.bind(host)
  host.getSourceFile = (path, ...args) => path === filename
    ? ts.createSourceFile(filename, text, options.target, true)
    : originalGetSourceFile(path, ...args)
  const program = ts.createProgram([filename], options, host)
  assert.deepEqual(ts.getPreEmitDiagnostics(program).map(diagnostic =>
    ts.flattenDiagnosticMessageText(diagnostic.messageText, '\n')), [])
})
