import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import test from 'node:test'
import { fileURLToPath } from 'node:url'

import { createGeaHostShims } from '../dist/host-shims.js'

const hostInclude = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../host/include')
const header = fs.readFileSync(path.join(hostInclude, 'host/performance.h'), 'utf8')

test('performance.now is a host namespace call on the performance facade', () => {
  const definitions = createGeaHostShims()
  assert.equal(definitions.hostGlobalObjects?.performance, 'gea::host::performance')
  assert.equal(definitions.hostGlobalObjectTypes?.performance, 'gea::host::PerformanceFacade')
  // A global object is a namespace root only when members are stated under it.
  assert.equal(definitions.hostNamespaceMethods?.performance?.now, 'gea::host::performance.now')
  assert.deepEqual(definitions.nativeNamespaceMethods?.performance?.now, {
    emit: 'gea::host::performance.now({args})',
    returnType: 'double',
    noThrow: true,
  })
  assert.deepEqual(definitions.hostNamespaceNoThrowMethods?.performance, ['now'])
  // Only now(): a member stated here and not defined in the header would
  // compile to a call nothing defines.
  assert.deepEqual(Object.keys(definitions.hostNamespaceMethods.performance), ['now'])
  assert.deepEqual(Object.keys(definitions.nativeNamespaceMethods.performance), ['now'])
})

test('a unit that names performance includes the header that declares it', () => {
  const declarations = createGeaHostShims().hostExternDeclarations
  for (const spelling of ['gea::host::performance', 'gea::host::performance.now']) {
    assert.ok(declarations?.[spelling]?.includes('#include "gea/embedded.h"'), `${spelling} has no declaration preamble`)
  }
  const umbrella = fs.readFileSync(path.join(hostInclude, 'gea/embedded-host.h'), 'utf8')
  assert.match(umbrella, /#include "host\/performance\.h"/)
  assert.match(header, /struct PerformanceFacade \{/)
  assert.match(header, /inline constexpr PerformanceFacade performance\{\};/)
  assert.match(header, /double now\(\) const/)
  // The same clock as Profiler.nowUs.
  assert.match(header, /#include "host\/profiler\.h"/)
  assert.match(header, /Profiler\.nowUs\(\)/)
})

// Needs a C++20 compiler: CXX, or clang++ on PATH. Skipped without one.
test('performance.now counts milliseconds from the start of the process', (t) => {
  const cxx = process.env.CXX || 'clang++'
  if (spawnSync(cxx, ['--version']).status !== 0) {
    t.skip(`no C++ compiler (${cxx})`)
    return
  }
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'gea-performance-'))
  try {
    const source = path.join(directory, 'performance.cpp')
    const binary = path.join(directory, process.platform === 'win32' ? 'performance.exe' : 'performance')
    fs.writeFileSync(
      source,
      `#include "host/performance.h"
#include <chrono>
#include <cstdio>
#include <thread>

int main() {
  const double first = gea::host::performance.now();
  const double profilerStart = gea::host::Profiler.nowUs();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const double second = gea::host::performance.now();
  const double profilerSpan = (gea::host::Profiler.nowUs() - profilerStart) / 1000.0;
  std::printf("%.6f %.6f %.6f\\n", first, second, profilerSpan);
  return 0;
}
`,
    )
    const built = spawnSync(cxx, ['-std=c++20', '-O1', `-I${hostInclude}`, source, '-o', binary], { encoding: 'utf8' })
    assert.equal(built.status, 0, built.stderr)
    const run = spawnSync(binary, [], { encoding: 'utf8' })
    assert.equal(run.status, 0, run.stderr)
    const [first, second, profilerSpan] = run.stdout.trim().split(' ').map(Number)
    // Near zero at the start, not the machine's uptime.
    assert.ok(first >= 0 && first < 1000, `first reading ${first} ms`)
    // Monotonic, in milliseconds: the 20 ms sleep reads as at least 20.
    assert.ok(second - first >= 19.5, `span ${second - first} ms`)
    // The same clock as Profiler: the two spans agree to within a millisecond.
    assert.ok(Math.abs(second - first - profilerSpan) < 1, `performance span ${second - first}, Profiler span ${profilerSpan}`)
  } finally {
    fs.rmSync(directory, { recursive: true, force: true })
  }
})
