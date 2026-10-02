import assert from 'node:assert/strict'
import test from 'node:test'
import { analyzeWorkerModules } from '../dist/analyze-workers.js'

test('discovers standard Worker and audioWorklet module URL edges', () => {
  assert.deepEqual(analyzeWorkerModules('app.ts', `
    const worker = new Worker(new URL('./worker.ts', import.meta.url), { type: 'module' })
    await context.audioWorklet.addModule(new URL('./capture.ts', import.meta.url))
  `), { realms: true, modules: ['./worker.ts', './capture.ts'], unknown: false })
})

test('string module paths and global constructors are recognized', () => {
  assert.deepEqual(analyzeWorkerModules('app.ts', `new globalThis.Worker('./worker.ts')`),
    { realms: true, modules: ['./worker.ts'], unknown: false })
})

test('computed module paths are conservatively unknown', () => {
  assert.deepEqual(analyzeWorkerModules('app.ts', `new Worker(path)`),
    { realms: true, modules: [], unknown: true })
})

test('comments and unrelated addModule methods do not enable worker realms', () => {
  assert.deepEqual(analyzeWorkerModules('app.ts', `
    // new Worker('./worker.ts')
    registry.addModule('./unrelated.ts')
  `), { realms: false, modules: [], unknown: false })
})

test('AudioWorkletNode and processor registration require isolated realms', () => {
  for (const code of [`new AudioWorkletNode(context, 'pcm')`, `registerProcessor('pcm', Processor)`])
    assert.equal(analyzeWorkerModules('app.ts', code).realms, true)
})

test('dependencies used only inside a worker infer network, TLS and audio', async () => {
  const { analyzeSourceHostBindings } = await import('../dist/analyze.js')
  const { fileURLToPath } = await import('node:url')
  const result = analyzeSourceHostBindings(fileURLToPath(new URL('./fixtures/worker-analysis/main.ts', import.meta.url)))
  assert.ok(result.features.includes('worker-realms'))
  assert.ok(result.features.includes('https'))
  assert.ok(result.bindings.includes('websocket'))
  assert.ok(result.bindings.includes('audio'))
})

test('type-only Worker imports do not shadow the browser constructor', () => {
  for (const declaration of [
    `import type { Worker } from './types'`,
    `import { type Worker } from './types'`,
    `import type Worker from './types'`,
    `interface Worker { stop(): void }`,
  ]) {
    assert.deepEqual(analyzeWorkerModules('app.ts', `${declaration}; new Worker('./worker.ts')`),
      { realms: true, modules: ['./worker.ts'], unknown: false })
  }
})

test('actual value bindings shadow browser constructors only in their lexical scope', () => {
  for (const code of [
    `import { Worker } from './custom'; new Worker('./not-a-module')`,
    `class Worker {}; new Worker('./not-a-module')`,
    `function f(Worker) { new Worker('./not-a-module') }`,
    `function f() { if (true) { var Worker } new Worker('./not-a-module') }`,
    `const globalThis = custom; new globalThis.Worker('./not-a-module')`,
  ]) assert.deepEqual(analyzeWorkerModules('app.ts', code), { realms: false, modules: [], unknown: false })
  assert.deepEqual(analyzeWorkerModules('app.ts', `function f(Worker) { new Worker('custom') }; new Worker('./real.ts')`),
    { realms: true, modules: ['./real.ts'], unknown: false })
})
