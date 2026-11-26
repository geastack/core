import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import { fileURLToPath } from 'node:url'
import vm from 'node:vm'
import test from 'node:test'
import ts from 'typescript'

const declarationPath = fileURLToPath(new URL('../index.d.ts', import.meta.url))
const typeSource = `
import type { CheckboxProps } from '@geastack/core'
const basic: CheckboxProps = { label: 'Notifications' }
const controlled: CheckboxProps = {
  label: 'Notifications',
  checked: false,
  indeterminate: true,
  disabled: false,
  class: { active: true },
  style: { gap: 8 },
  onChange(next) { const value: boolean = next },
  onPress(event) { const type: string = event.type },
  onClick(event) { const type: string = event.type },
}
// @ts-expect-error A visible label is required.
const missingLabel: CheckboxProps = {}
`
const options = {
  noEmit: true,
  strict: true,
  skipLibCheck: true,
  target: ts.ScriptTarget.ES2022,
  module: ts.ModuleKind.NodeNext,
  moduleResolution: ts.ModuleResolutionKind.NodeNext,
  paths: { '@geastack/core': [declarationPath] },
}
const host = ts.createCompilerHost(options)
const readSource = host.getSourceFile.bind(host)
host.getSourceFile = (path, ...args) => path === '/checkbox-props.ts'
  ? ts.createSourceFile(path, typeSource, options.target, true)
  : readSource(path, ...args)
const program = ts.createProgram(['/checkbox-props.ts'], options, host)
const diagnostics = ts.getPreEmitDiagnostics(program).map(diagnostic =>
  ts.flattenDiagnosticMessageText(diagnostic.messageText, '\n'))
assert.deepEqual(diagnostics, [])

const componentPath = fileURLToPath(new URL('../../elements/components/Checkbox.tsx', import.meta.url))
const source = await readFile(componentPath, 'utf8')
const compiled = ts.transpileModule(source, {
  compilerOptions: {
    target: ts.ScriptTarget.ES2022,
    module: ts.ModuleKind.CommonJS,
    jsx: ts.JsxEmit.React,
    jsxFactory: 'jsx',
  },
}).outputText

const moduleExports = {}
vm.runInNewContext(compiled, {
  exports: moduleExports,
  jsx: (type, props, ...children) => ({
    type,
    props: { ...props, children },
  }),
})
const { Checkbox } = moduleExports

function render(props = {}) {
  return Checkbox({ label: 'Example option', ...props })
}

test('unchecked Checkbox requests checked=true and invokes each callback once', () => {
  const changes = []
  const callbacks = []
  const event = { type: 'click' }
  const view = render({
    onChange: (value) => changes.push(value),
    onPress: (value) => callbacks.push(['press', value]),
    onClick: (value) => callbacks.push(['click', value]),
  })

  view.props.onClick(event)

  assert.deepEqual(changes, [true])
  assert.deepEqual(callbacks, [['press', event], ['click', event]])
})

test('checked Checkbox requests checked=false', () => {
  const changes = []
  render({ checked: true, onChange: (value) => changes.push(value) }).props.onClick({})
  assert.deepEqual(changes, [false])
})

test('indeterminate Checkbox requests checked=true', () => {
  const changes = []
  render({ indeterminate: true, onChange: (value) => changes.push(value) }).props.onClick({})
  assert.deepEqual(changes, [true])
})

test('disabled Checkbox suppresses state and interaction callbacks', () => {
  const calls = []
  const view = render({
    disabled: true,
    onChange: (value) => calls.push(['change', value]),
    onPress: () => calls.push(['press']),
    onClick: () => calls.push(['click']),
  })

  view.props.onClick({ type: 'click' })
  assert.deepEqual(calls, [])
})

test('registers one native click binding for activation', () => {
  const view = render({ onPress() {}, onClick() {} })
  assert.equal(typeof view.props.onClick, 'function')
  assert.equal('onPress' in view.props, false)
})

test('does not expose unsupported native accessibility or keyboard props', () => {
  const view = render({ checked: true, disabled: true, indeterminate: true })
  assert.equal('role' in view.props, false)
  assert.equal('aria-checked' in view.props, false)
  assert.equal('aria-disabled' in view.props, false)
  assert.equal('aria-label' in view.props, false)
  assert.equal('tabIndex' in view.props, false)
  assert.equal('onKeyDown' in view.props, false)
})

test('passes ClassValue through and supplies overridable default styles', () => {
  const customClass = { 'custom-active': true, 'custom-off': false }
  const view = render({ checked: true, class: customClass, style: { gap: 16 } })
  assert.equal(view.props.class, customClass)
  assert.equal(view.props.style.display, 'flex')
  assert.equal(view.props.style.gap, 16)
  assert.equal(view.props.children[0].props.style.width, 18)
})

test('requires a non-empty visible label for every Checkbox', () => {
  assert.throws(() => Checkbox({ label: '' }), /non-empty label/)
  assert.throws(() => Checkbox({}), /non-empty label/)
})
