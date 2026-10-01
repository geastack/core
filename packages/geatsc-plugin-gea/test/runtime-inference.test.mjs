import assert from 'node:assert/strict'
import test from 'node:test'
import { inferCanvasOnly } from '../dist/analyze-runtime.js'
import { analyzeSourceHostBindings } from '../dist/analyze.js'

const canvas = `import { Display as Panel, rgb } from '@geastack/core';
const ctx = Panel.ctx;
function frame(t: number) { ctx.fillRect(0, 0, window.innerWidth, window.innerHeight); requestAnimationFrame(frame); }
requestAnimationFrame(frame);`
const infer = (source = canvas, extra = {}, unknown = false) => inferCanvasOnly(new Map([
  ['/app/index.tsx', source], ...Object.entries(extra).map(([name, text]) => ['/app/' + name, text]),
]), unknown)

test('direct display apps are inferred without app identity or options', () => {
  assert.equal(infer(), true)
  assert.equal(infer(canvas.replace('Panel', 'RenamedDisplay').replaceAll('Panel.', 'RenamedDisplay.')), true)
  assert.equal(infer(canvas + '\nconst screen = { width: 12 }; console.log(screen.width)'), true)
  assert.equal(infer(canvas + '\nclass App { fetch() { return 1 } }; new App().fetch()'), true)
  assert.equal(infer(canvas + '\nconst document = { x: 1 }; console.log(document.x)'), true)
  assert.equal(infer(canvas, {}, true), false)
})

for (const extra of [
  'const ui = <div/>', 'const ui = <canvas/>', 'document.createElement("canvas")',
  'const d = document; d.body.appendChild(x)', 'const { document: d } = window',
  'const w = window; w.document', 'window["document"]', 'globalThis[key]',
  'const { document } = globalThis', 'fetch("/data")', 'new WebSocket("ws://localhost")',
  'localStorage.setItem("x", "1")', 'unknownHostFunction()', 'eval(code)',
  'new Function(code)()', 'import("./extra")',
  `import * as Gea from '@geastack/core'`, `import { mount as start } from '@geastack/core'`,
  `import { Audio } from '@geastack/core'`, `export { mount } from '@geastack/core'`,
  'const obj = { document }', 'declare const hiddenHost: () => void; hiddenHost()',
  'Panel.setAutoRotation(true)', 'const p = Panel; p[operation]()',
]) test('full runtime is retained for ' + extra, () => assert.equal(infer(canvas + '\n' + extra), false))

test('dependency code, CSS, and unresolved graphs prevent unsafe UI removal', () => {
  assert.equal(infer(canvas, { 'helper.ts': 'document.body.innerHTML = "hello"' }), false)
  assert.equal(infer(canvas, { 'style.css': 'body { color: red }' }), false)
  assert.equal(infer(canvas, { 'bad.ts': 'const = ' }), false)
  assert.equal(infer('console.log(1)'), false)
  assert.equal(infer(canvas + `\nimport type { HTMLElement } from '@geastack/core'`), true)
})

test('real canvas and JSX examples expose opposite runtime proofs', { skip: !process.env.GEA_TEST_EXAMPLES_ROOT }, () => {
  const root = process.env.GEA_TEST_EXAMPLES_ROOT
  assert.ok(analyzeSourceHostBindings(root + '/apps/canvas-3d/index.tsx').features.includes('runtime-canvas-only'))
  assert.ok(!analyzeSourceHostBindings(root + '/apps/bouncing-balls-jsx/index.tsx').features.includes('runtime-canvas-only'))
})
