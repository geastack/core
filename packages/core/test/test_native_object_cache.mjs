import assert from 'node:assert/strict'
import { mkdirSync, statSync, utimesSync, writeFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { spawnSync } from 'node:child_process'
import test from 'node:test'

const root = resolve(import.meta.dirname, '../../..')
const buildDir = resolve(root, 'packages/core/test/.build')
const source = resolve(buildDir, 'native-object-cache-test.cpp')
const header = resolve(buildDir, 'native-object-cache-test.h')
const output = resolve(buildDir, 'native-object-cache-test')

test('native object cache detects source, header and object changes within one second', () => {
  mkdirSync(buildDir, { recursive: true })
  writeFileSync(header, '#define CACHE_TEST_RESULT 0\n')
  writeFileSync(source, '#include "native-object-cache-test.h"\nint main() { return CACHE_TEST_RESULT; }\n')

  function build() {
    const result = spawnSync('bash', ['-c', `
      set -euo pipefail
      source "$1/packages/core/scripts/native-object-cache.sh"
      export TMPDIR="$2"
      GEA_NATIVE_CACHE_BASE_DIR="$1"
      GEA_NATIVE_CACHE_OBJ_ROOT="$2/.build/obj"
      GEA_NATIVE_CACHE_OUTPUT="$3"
      GEA_NATIVE_CXX_BIN="$CXX"
      GEA_NATIVE_CCACHE=0
      GEA_NATIVE_JOBS=1
      GEA_NATIVE_CXX_SOURCES=("$4")
      GEA_NATIVE_CXXFLAGS=(-std=c++20)
      gea_native_compile_and_link
      printf '%s\\n' "$GEA_NATIVE_OBJECTS"
    `, 'native-cache-test', root, buildDir, output, source], {
      encoding: 'utf8',
      env: { ...process.env, CXX: process.env.CXX || 'clang++' },
    })
    assert.equal(result.status, 0, result.stdout + result.stderr)
    return { object: result.stdout.trim(), log: result.stderr }
  }

  function run(expected) {
    const result = spawnSync(output, [], { encoding: 'utf8' })
    assert.equal(result.status, expected, result.stdout + result.stderr)
  }

  const first = build()
  run(0)
  const second = Math.floor(Date.now() / 1000) - 10
  const timestamp = (path, fraction) => utimesSync(path, second + fraction, second + fraction)
  function primeTimes() {
    timestamp(source, 0.1)
    timestamp(header, 0.1)
    timestamp(first.object, 0.5)
    timestamp(output, 0.8)
  }

  primeTimes()
  assert.match(build().log, /compiled=0 reused=1 linked=0/)

  writeFileSync(source, '#include "native-object-cache-test.h"\nint main() { return CACHE_TEST_RESULT + 1; }\n')
  timestamp(source, 0.9)
  assert.ok(statSync(source).mtimeMs > statSync(first.object).mtimeMs)
  assert.match(build().log, /compiled=1 reused=0 linked=1/)
  run(1)

  primeTimes()
  writeFileSync(header, '#define CACHE_TEST_RESULT 2\n')
  timestamp(header, 0.9)
  assert.match(build().log, /compiled=1 reused=0 linked=1/)
  run(3)

  primeTimes()
  timestamp(first.object, 0.9)
  assert.match(build().log, /compiled=0 reused=1 linked=1/)
  run(3)
})
