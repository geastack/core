// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { execFileSync, spawnSync } from 'node:child_process'
import { readFileSync, existsSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { resolve } from 'node:path'
import * as manifest from '../gea_sources.mjs'
const core = fileURLToPath(new URL('../../../', import.meta.url))
const workspace = resolve(core, '..')
const apple = resolve(workspace, 'apple/packages/geastack-apple')
const env = {
  ...process.env,
  GEA_CORE: resolve(core, 'packages/core'), GEA_HOST_DIR: resolve(core, 'packages/host'),
  GEA_ENGINE_DIR: resolve(core, 'packages/engine'), GEA_ELEMENTS_DIR: resolve(core, 'packages/elements'),
  GEA_GEAOS_PACKAGE_DIR: resolve(core, 'packages/geaos'), GEA_DESKTOP_WEBSOCKET_CURL: ''
}
// The manifest joins its package directories with `/` on every host (see
// gea_sources.mjs), so a Windows directory keeps its backslashes up to there.
const curl = `${env.GEA_HOST_DIR}/host/websocket_curl.cpp`
const nativeApple = `${env.GEA_HOST_DIR}/host/websocket_apple.mm`
const count = (list, name) => list.filter(p => p === name).length
assert.equal(count(manifest.cxxSources(env), curl), 0, 'unrelated framework/browser/unit builds must not acquire libcurl')
assert.equal(count(manifest.cxxSources({ ...env, GEA_DESKTOP_WEBSOCKET_CURL: '1' }), curl), 1,
  'native Linux source selection must link the real curl provider exactly once')
assert.equal(typeof manifest.objcxxSources, 'function', 'Apple has a shared framework provider manifest')
assert.deepEqual(manifest.objcxxSources(env), [nativeApple])
assert.ok(existsSync(nativeApple) && existsSync(curl))
const shell = (body, extra = {}) => execFileSync('bash', ['-c', body], {
  encoding: 'utf8', timeout: 15000, env: { ...env, ...extra }
}).trim().split('\n').filter(Boolean)
const setup = '. "$GEA_CORE/gea_sources.sh"\n'
assert.deepEqual(shell(setup + 'gea_fw_objcxx_sources'), [nativeApple], 'real shell consumer emits Apple source')
assert.equal(count(shell(setup + 'GEA_DESKTOP_WEBSOCKET_CURL=1 gea_fw_cxx_sources'), curl), 1)
// Execute the actual target array-selection blocks, not an authored stand-in.
const appleBuild = readFileSync(resolve(apple, 'targets/macos/build-macos.sh'), 'utf8')
const appleStart = appleBuild.indexOf('\nMM_SOURCES=(')
const appleEnd = appleBuild.indexOf('\nif [[ "${GEA_NATIVE_DEBUGGER:-0}"', appleStart)
assert.ok(appleStart >= 0 && appleEnd > appleStart)
const selectedApple = shell(setup + appleBuild.slice(appleStart, appleEnd) + '\nprintf "%s\\n" "${MM_SOURCES[@]}"', { ROOT_DIR: apple })
assert.equal(count(selectedApple, nativeApple), 1, 'actual Apple compile array includes the strong provider')
assert.equal(count(selectedApple, curl), 0)
for (const replacement of ['return 1', 'return 0']) {
  const refused = spawnSync('bash', ['-c', setup + `gea_fw_objcxx_sources() { ${replacement}; }\n` + appleBuild.slice(appleStart, appleEnd) + '\nprintf "UNREACHABLE\n"'], {
    encoding: 'utf8', timeout: 15000, env: { ...env, ROOT_DIR: apple }
  })
  assert.ifError(refused.error)
  assert.equal(refused.signal, null)
  assert.equal(refused.status, 1, 'failed/empty source query must stop Apple build')
  assert.ok(!refused.stdout.includes('UNREACHABLE'))
}

const linuxBuild = readFileSync(resolve(workspace, 'linux/targets/raspberry-pi-os/build-raspberry-pi-os.sh'), 'utf8')
const linuxStart = linuxBuild.indexOf('\nCXX_SOURCES=()')
const linuxEnd = linuxBuild.indexOf('\nCXX_SOURCES+=(', linuxStart)
assert.ok(linuxStart >= 0 && linuxEnd > linuxStart)
const selectedLinux = shell(setup + linuxBuild.slice(linuxStart, linuxEnd) + '\nprintf "%s\\n" "${CXX_SOURCES[@]}"')
assert.equal(count(selectedLinux, curl), 1, 'actual Raspberry Pi compile array opts into curl provider')
const oldCore = spawnSync('bash', ['-c', setup + 'gea_fw_cxx_sources() { printf "/old/host/websocket.cpp\\n"; }\n' + linuxBuild.slice(linuxStart, linuxEnd) + '\nprintf "UNREACHABLE\n"'], {
  encoding: 'utf8', timeout: 15000, env
})
assert.ifError(oldCore.error)
assert.equal(oldCore.signal, null)
assert.equal(oldCore.status, 1, 'old core without provider must not produce an app with weak hooks')
assert.match(oldCore.stderr, /Missing framework Linux WebSocket transport source/)
assert.ok(!oldCore.stdout.includes('UNREACHABLE'))

assert.match(linuxBuild, /source "\$GEA_CORE\/gea_sources\.sh"/, 'Raspberry Pi uses the current package manifest path')
assert.match(linuxBuild, /export GEA_CORE GEA_HOST_DIR GEA_ENGINE_DIR GEA_ELEMENTS_DIR GEA_GEAOS_PACKAGE_DIR/)
// Sailfish stages source files before calling the same opt-in manifest.
const sailfish = readFileSync(resolve(workspace, 'linux/targets/sailfish-os/prepare.mjs'), 'utf8')
assert.match(sailfish, /cxxSources\(\{\s*\.\.\.env,\s*GEA_DESKTOP_WEBSOCKET_CURL:\s*'1'\s*\}\)/)
const cmake = readFileSync(resolve(workspace, 'linux/targets/sailfish-os/CMakeLists.txt'), 'utf8')
assert.match(cmake, /PkgConfig::CURL/, 'Sailfish real target links curl')
assert.match(linuxBuild, /-lcurl/, 'Raspberry Pi real target links curl')
console.log('PASS native Apple/Linux provider manifests and actual target source arrays; unrelated default sources unchanged')
