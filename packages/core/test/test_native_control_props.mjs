import assert from 'node:assert/strict'
import { fileURLToPath } from 'node:url'
import test from 'node:test'
import ts from 'typescript'

test('native buttons and inputs retain typed IDs and data attributes', () => {
  const filename = '/native-control-props.ts'
  const declaration = fileURLToPath(new URL('../index.d.ts', import.meta.url))
  const source = `
import type { NativeButtonProps, NativeInputElementProps, NativeSelectProps, NativeOptionProps, NativeStepperProps, NativeProgressProps, NativeTextAreaElementProps } from '@geastack/core'
const button: NativeButtonProps = { id: 'action', 'data-purpose': 'sample', onClick() {} }
const input: NativeInputElementProps = { id: 'switch', 'data-enabled': true, type: 'checkbox', checked: true }
const radio: NativeInputElementProps = { type: 'radio', name: 'group', value: 'first', disabled: false }
const select: NativeSelectProps = { id: 'menu', value: 'first', onInput(event) { const value: string = event.currentTarget.value } }
const option: NativeOptionProps = { value: 'first', selected: true, disabled: false }
const stepper: NativeStepperProps = { value: '3', min: 1, max: 12, step: 1, onInput() {} }
const progress: NativeProgressProps = { value: 64, max: 100, indeterminate: false }
const notes: NativeTextAreaElementProps = { id: 'notes', value: '', disabled: true, 'data-purpose': 'editor' }
// @ts-expect-error Selection flags remain boolean.
const badOption: NativeOptionProps = { selected: 'yes' }
// @ts-expect-error IDs remain strings.
const badButton: NativeButtonProps = { id: 42 }
// @ts-expect-error Inputs do not accept arbitrary properties.
const badInput: NativeInputElementProps = { mystery: true }
`
  const options = {
    noEmit: true,
    strict: true,
    skipLibCheck: true,
    target: ts.ScriptTarget.ES2022,
    module: ts.ModuleKind.NodeNext,
    moduleResolution: ts.ModuleResolutionKind.NodeNext,
    paths: { '@geastack/core': [declaration] },
  }
  const host = ts.createCompilerHost(options)
  const readSource = host.getSourceFile.bind(host)
  host.getSourceFile = (path, ...args) => path === filename
    ? ts.createSourceFile(filename, source, options.target, true)
    : readSource(path, ...args)
  const program = ts.createProgram([filename], options, host)
  assert.deepEqual(ts.getPreEmitDiagnostics(program).map(diagnostic =>
    ts.flattenDiagnosticMessageText(diagnostic.messageText, '\n')), [])
})
