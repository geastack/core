import assert from 'node:assert/strict'
import fs, { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { runInNewContext } from 'node:vm'
import test from 'node:test'
import ts from 'typescript'
import { createGeaHostShims } from '../dist/host-shims.js'
import { analyzeSourceHostBindings } from '../dist/analyze.js'

const core = resolve(import.meta.dirname, '../../core')
const source = ts.createSourceFile('host.ts', readFileSync(resolve(core, 'runtime/host.ts'), 'utf8'), ts.ScriptTarget.Latest, true)
const declaration = source.statements.find(statement => ts.isVariableStatement(statement) && statement.declarationList.declarations.some(d => d.name.getText(source) === 'Magnetometer'))
const code = ts.transpileModule(declaration.getText(source).replace('export const Magnetometer', 'globalThis.Magnetometer'), { compilerOptions: { target: ts.ScriptTarget.ES2022 } }).outputText

test('indexed magnetometer samples preserve saturation, timestamps and validity', () => {
  const context = { __gea_Magnetometer: {
    open() { return true }, close() {}, count() { return 2 },
    read(index, values) {
      if (index !== 1) return false
      values.set([-32768, 140, 22, 1, 123456])
      return true
    },
  } }
  runInNewContext(code, context)
  assert.equal(context.Magnetometer.open(), true)
  assert.equal(context.Magnetometer.count(), 2)
  assert.deepEqual({ ...context.Magnetometer.read(1) }, { x: -32768, y: 140, z: 22, valid: true, saturatedAxes: 1, timestampUs: 123456 })
  assert.equal(context.Magnetometer.read(2).valid, false)
  context.Magnetometer.close()
})

test('absent magnetometers are unavailable and typed views preserve runtime checks', () => {
  const context = {}
  runInNewContext(code, context)
  assert.equal(context.Magnetometer.open(), false)
  assert.equal(context.Magnetometer.count(), 0)
  assert.deepEqual({ ...context.Magnetometer.read(0) }, { x: 0, y: 0, z: 0, valid: false, saturatedAxes: 0, timestampUs: 0 })
  context.Magnetometer.close()
  const bindings = createGeaHostShims()
  assert.equal(bindings.hostNamespaces.__gea_Magnetometer, 'gea::host::Magnetometer')
  assert.equal(bindings.hostNamespaces.Magnetometer, undefined)
  for (const name of ['open', 'close', 'count', 'read'])
    assert.equal(bindings.hostNamespaceMethods.__gea_Magnetometer[name], `gea::host::Magnetometer.${name}`)
  assert.ok(!bindings.hostNamespaceNoThrowMethods.__gea_Magnetometer.includes('read'))
})

for (const input of [
  "import { Magnetometer } from '@geastack/core'; Magnetometer.open()",
  '__gea_Magnetometer.read(0, new Float64Array(5))',
]) test('magnetometer calls report their sensor host binding: ' + input, t => {
  // The analyzer resolves its entry; a drive-relative path gains a drive letter on Windows.
  const entry = resolve('/virtual-magnetometer-api.ts')
  t.mock.method(fs, 'existsSync', file => file === entry)
  t.mock.method(fs, 'statSync', () => ({ isFile: () => true }))
  t.mock.method(fs, 'readFileSync', () => input)
  const analysis = analyzeSourceHostBindings(entry)
  assert.deepEqual(analysis.bindings, ['imu'])
})
