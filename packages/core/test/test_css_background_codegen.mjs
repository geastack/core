import assert from 'node:assert/strict'
import fs from 'node:fs'
import { createRequire } from 'node:module'
import vm from 'node:vm'
import test from 'node:test'

// Exercise the real registration emitter without invoking the application build.
const require = createRequire(new URL('../../geatsc-plugin-gea/package.json', import.meta.url))
const ts = require('typescript')
const source = fs.readFileSync(new URL('../scripts/build-gea-vite-geatsc.mjs', import.meta.url), 'utf8')
const parsed = ts.createSourceFile('build-gea-vite-geatsc.mjs', source, ts.ScriptTarget.Latest, true, ts.ScriptKind.JS)
const functions = parsed.statements.filter(ts.isFunctionDeclaration).map(fn => fn.getText(parsed)).join('\n')
const constants = parsed.statements.filter(node => ts.isVariableStatement(node) && node.declarationList.declarations.every(decl => /^static/.test(decl.name.getText(parsed)) && decl.name.getText(parsed) !== 'staticCssTapeMinChunk')).map(node => node.getText(parsed)).join('\n')
const context = vm.createContext({ useStaticCssRules: true })
vm.runInContext(functions + '\n' + constants, context)
const emit = (name, value, media = null) => context.emitCssRuleRegistrations('class', 'probe', name, value, media).join('\n')

test('static color registrations distinguish longhand and shorthand', () => {
  assert.match(emit('background-color', '#ff0000'), /StaticStyleColorProperty::BackgroundColor,/)
  assert.match(emit('background', '#ff0000'), /StaticStyleColorProperty::Background,/)
  assert.match(emit('background-color', 'var(--paper, #ff0000)'), /StaticStyleColorProperty::BackgroundColor,/)
})

test('static gradient registrations preserve image-only semantics and media', () => {
  assert.match(emit('background-image', 'linear-gradient(#ff0000, #0000ff)'), /registerStaticBackgroundRule\([^;]*, nullptr, true\);/)
  assert.match(emit('background', 'linear-gradient(#ff0000, #0000ff)'), /registerStaticBackgroundRule\([^;]*, nullptr, false\);/)
  assert.match(emit('background-image', 'linear-gradient(var(--ink, #ff0000), #0000ff)', '(min-width: 200px)'), /registerStaticBackgroundFullRule\([^;]*, "\(min-width: 200px\)", true\);/)
})

test('combined image/color shorthand keeps its color in the full declaration parser', () => {
  const code = emit('background', 'linear-gradient(transparent, transparent) #00ff00')
  assert.doesNotMatch(code, /registerStaticBackground(?:Full)?Rule/)
  assert.match(code, /"background", "linear-gradient\(transparent, transparent\) #00ff00"/)
})


test('background clip boxes and empty image layers retain the full parser metadata', () => {
  for (const [property, value] of [
    ['background', 'linear-gradient(#ff0000, #0000ff) content-box'],
    ['background-image', 'linear-gradient(#ff0000, #0000ff), none'],
  ]) {
    const code = emit(property, value)
    assert.doesNotMatch(code, /registerStaticBackground(?:Full)?Rule/)
    assert.ok(code.includes(JSON.stringify(value)))
  }
})

// The background content-box reftest also relies on flex shorthand metadata.
test('static flex registrations preserve percentage bases and shrink factors', () => {
  assert.match(emit('flex', '0 1 50%'), /registerStaticFlexRule\([^;]*StaticStyleLengthUnit::Percent, 50[^;]*true/)
  assert.match(emit('flex-basis', '50%'), /StaticStyleLengthProperty::FlexBasis, [^;]*StaticStyleLengthUnit::Percent, 50/)
  for (const value of ['0 0 50%', 'none', 'auto', '0 0 calc(50% - 10px)']) {
    const code = emit('flex', value)
    assert.doesNotMatch(code, /registerStaticFlexRule/)
    assert.ok(code.includes(JSON.stringify(value)), code)
  }
})

test('line-height relative units reach the runtime expression parser intact', () => {
  for (const [name, value] of [['width', '2lh'], ['height', '1rlh'], ['line-height', 'calc(1lh + 2px)'], ['font', '20px/2lh Ahem']]) {
    const code = emit(name, value)
    assert.ok(code.includes(JSON.stringify(name)), `${name}: ${value} => ${code}`)
    assert.ok(code.includes(JSON.stringify(value)), code)
    assert.doesNotMatch(code, /registerStatic(?:Length|Property)Rule/)
  }
})

test('static transform-style registrations reach the runtime parser', () => {
  assert.match(emit('transform-style', 'preserve-3d'), /transform-style/)
  assert.match(emit('transform-style', 'flat'), /transform-style/)
})

test('background placement keeps complete layer lists and one-value size auto height', () => {
  assert.match(emit('background-size', '20px'), /StaticStyleLengthUnit::Px, 20[^;]*StaticStyleLengthUnit::Auto, 0/)
  for (const [name, value] of [['background-size', '20px 30px, 40px 50px'],
    ['background-position', '50px 50px, 0 0'], ['background-attachment', 'scroll, fixed'],
    ['background-repeat', 'repeat-x, no-repeat'], ['background-origin', 'padding-box, content-box']]) {
    const code = emit(name, value)
    assert.ok(code.includes(JSON.stringify(value)), code)
    assert.doesNotMatch(code, /StyleDeclaration::Ignored/)
  }
})
