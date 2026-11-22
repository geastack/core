import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { createRequire } from 'node:module'
import { fileURLToPath } from 'node:url'

const require = createRequire(import.meta.url)
const ts = require('typescript')
const runtimeFile = new URL('../runtime/compiler.ts', import.meta.url)
const javascript = ts.transpileModule(readFileSync(runtimeFile, 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ESNext }
}).outputText
const { Component, ReactiveComponent } = await import('data:text/javascript;base64,' + Buffer.from(javascript).toString('base64'))
assert.equal(new Component().onAfterRender(), undefined)
assert.equal(new ReactiveComponent().onAfterRender(), undefined)

// An override must type-check against both the public API and compiler runtime.
for (const specifier of ['../index', '../runtime/compiler']) {
  // TypeScript hands the host forward-slash paths on every platform, so the
  // in-memory root is named in that spelling too (a no-op on POSIX).
  const filename = ts.normalizePath(fileURLToPath(new URL('./component-startup-check.ts', import.meta.url)))
  const source = `import { ReactiveComponent } from '${specifier}'
class Example extends ReactiveComponent {
  override onAfterRender(): void { super.onAfterRender() }
}
new Example().onAfterRender()`
  const options = { strict: true, noImplicitOverride: true, noEmit: true, skipLibCheck: true,
    target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ESNext, moduleResolution: ts.ModuleResolutionKind.Bundler }
  const host = ts.createCompilerHost(options)
  const getSourceFile = host.getSourceFile.bind(host)
  host.getSourceFile = (file, languageVersion, ...rest) => file === filename
    ? ts.createSourceFile(file, source, languageVersion, true)
    : getSourceFile(file, languageVersion, ...rest)
  const program = ts.createProgram([filename], options, host)
  const diagnostics = ts.getPreEmitDiagnostics(program)
  assert.equal(diagnostics.length, 0, ts.formatDiagnosticsWithColorAndContext(diagnostics, {
    getCanonicalFileName: file => file, getCurrentDirectory: () => process.cwd(), getNewLine: () => '\n'
  }))
}
console.log('Component startup: public/runtime overrides and inherited base hook passed')
