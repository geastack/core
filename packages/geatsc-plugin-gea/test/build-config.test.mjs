import assert from 'node:assert/strict'
import test from 'node:test'
import { generateCppIrSource, generateCppIrNamespaceSource } from '../dist/cpp-ir.js'

const ir = { schema: 'gea-ir', version: 1, entry: '/canvas.ts', modules: [], components: [], stores: [], templates: [], hostCapabilities: ['canvas'] }

test('canvas emission is controlled by the explicit option, independently on each compilation', () => {
  const old = process.env.GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
  try {
    process.env.GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT = '1'
    const full = generateCppIrSource(ir, null, [])
    const direct = generateCppIrSource(ir, null, [], 'gea::framework::app::generated', true)
    assert.doesNotMatch(full, /#include "direct_canvas_runtime.h"/)
    assert.match(direct, /#include "direct_canvas_runtime.h"/)
    assert.equal(generateCppIrSource(ir, null, []), full)
    const fullNamespace = generateCppIrNamespaceSource(ir, [], { includeDomInterop: true })
    const directNamespace = generateCppIrNamespaceSource(ir, [], { includeDomInterop: true, directCanvas: true })
    assert.notEqual(fullNamespace, directNamespace)
    process.env.GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT = '0'
    assert.equal(generateCppIrNamespaceSource(ir, [], { includeDomInterop: true, directCanvas: true }), directNamespace)
  } finally {
    if (old === undefined) delete process.env.GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
    else process.env.GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT = old
  }
})
