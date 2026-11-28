import fs from 'node:fs'
import path from 'node:path'
import { execFileSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'

/** Honor an app's declared compiler plugin; only legacy undeclared apps inherit Core's. */
function resolveImport(directory) {
  // Let Node apply real ESM export conditions (the plugin is import-only).
  // A loaded build machine can take well over 10 s just to start Node.
  const url = execFileSync(process.execPath,
    ['--input-type=module', '-e', "process.stdout.write(import.meta.resolve('@geajs/vite-plugin'))"],
    { cwd: directory, encoding: 'utf8', timeout: 120000, stdio: ['ignore', 'pipe', 'pipe'] })
  return fs.realpathSync(fileURLToPath(url))
}

function workspaceOwner(appDir) {
  const app = path.resolve(appDir)
  if (fs.existsSync(path.join(app, 'node_modules', '@geajs', 'vite-plugin'))) return null
  for (let dir = path.dirname(app); dir !== path.dirname(dir); dir = path.dirname(dir)) {
    let manifest
    try { manifest = JSON.parse(fs.readFileSync(path.join(dir, 'package.json'), 'utf8')) } catch { continue }
    const patterns = Array.isArray(manifest.workspaces) ? manifest.workspaces : manifest.workspaces?.packages ?? []
    const relative = path.relative(dir, app).split(path.sep).join('/')
    if (patterns.some(pattern => path.posix.matchesGlob(relative, pattern.replace(/\/+$/, '')))) return dir
  }
  return null
}

export function resolveAppGeaVitePlugin(appDir, corePackageJson) {
  const manifest = path.join(appDir, 'package.json')
  // A bare fixture directory has no manifest and so declares no plugin of its own.
  const app = fs.existsSync(manifest) ? JSON.parse(fs.readFileSync(manifest, 'utf8')) : {}
  const declared = ['dependencies', 'devDependencies', 'optionalDependencies', 'peerDependencies']
    .some(field => Object.hasOwn(app[field] || {}, '@geajs/vite-plugin'))
  if (!declared) return resolveImport(path.dirname(corePackageJson instanceof URL ? fileURLToPath(corePackageJson) : corePackageJson))
  // Resolving from the app may otherwise walk up to an unrelated ancestor install.
  // Require an actual app-owned installation entry, or the hoisted one of an npm
  // workspace root that lists this app among its workspaces.
  const installed = path.join(workspaceOwner(appDir) ?? appDir, 'node_modules', '@geajs', 'vite-plugin')
  try {
    const packageRoot = fs.realpathSync(installed)
    const actual = JSON.parse(fs.readFileSync(path.join(packageRoot, 'package.json'), 'utf8'))
    if (actual.name !== '@geajs/vite-plugin') throw new Error('installed package name mismatch')
    const resolved = resolveImport(appDir)
    if (!resolved.startsWith(packageRoot + path.sep)) throw new Error('resolved outside declared installation')
    return resolved
  } catch (error) {
    throw new Error(`App declares @geajs/vite-plugin but its installation is unavailable: ${error.message}`)
  }
}
