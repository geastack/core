#!/usr/bin/env node
// Shared geastack framework source manifest — the SINGLE SOURCE OF TRUTH for the
// always-linked framework C/C++ source set and include roots. Target builds
// (simulator build-web.sh, and the esp32/geaos CMake) read this instead of
// hardcoding source lists. As files move between the new packages
// (core/host/engine/elements/geaos) during the decomposition, ONLY this file
// changes and every target follows. See docs/package-architecture-plan.md.
//
// This is Node rather than shell because CMake has to read it on Windows too,
// where there is no bash: `execute_process(COMMAND bash -c ...)` failed
// outright and no ESP32 target could configure. Node is already a hard
// requirement of every path that reaches here (the CLI and geatsc are Node),
// so it costs no new dependency. gea_sources.sh remains as a thin wrapper for
// the shell callers that source it.
//
// Caller must export GEA_CORE = the @geastack/core package dir. In a normal npm
// install the CLI resolves every package through Node's npm layout and exports
// its exact directory before invoking a native build.
// ALL package include roots are on every -I line (flat `ui/…`/`host/…` spellings
// preserved) so a header resolves wherever its package put it.
//
// NOTE: app-specific sources (gea_app_entry and generated program/font/assets)
// and target shims (web_*, esp_*) are NOT here — they stay in the target build.
// This manifest is only the always-linked shared framework.

import path from 'node:path'
import { fileURLToPath } from 'node:url'

// Paths are joined with `/` rather than path.join: the output feeds CMake and
// compiler command lines, which take forward slashes on every platform, and it
// keeps the emitted strings byte-identical to what the shell manifest produced.
const join = (...parts) => parts.join('/')

function packageDirs(env) {
  const read = (name) => {
    const value = env[name]
    if (!value) throw new Error(`${name} unset`)
    return value
  }
  return {
    core: read('GEA_CORE'),
    host: read('GEA_HOST_DIR'),
    engine: read('GEA_ENGINE_DIR'),
    elements: read('GEA_ELEMENTS_DIR'),
    geaos: read('GEA_GEAOS_PACKAGE_DIR')
  }
}

export function includeFlags(env = process.env) {
  const { core, host, engine, elements, geaos } = packageDirs(env)
  return [
    join(core, 'include'),
    core,
    join(host, 'include'),
    host,
    engine,
    join(engine, 'ui'),
    elements,
    join(elements, 'ui'),
    geaos,
    join(engine, 'vendor/stb'),
    join(engine, 'vendor/AnimatedGIF')
  ].map((dir) => `-I${dir}`)
}

export function cSources(env = process.env) {
  const { engine } = packageDirs(env)
  return [join(engine, 'vendor/AnimatedGIF/AnimatedGIF.c')]
}

export function cxxSources(env = process.env) {
  const { core, host, engine, elements, geaos } = packageDirs(env)
  const cpp = (dir, names) => names.map((name) => join(dir, `${name}.cpp`))
  return [
    // @geastack/engine — machinery + intrinsics + text-input/keyboard runtime
    ...cpp(join(engine, 'ui'), [
      'tree_state', 'node_lifecycle', 'display_invalidation', 'layout_snapshot',
      'absolute_leaf_refresh', 'viewport_region', 'dirty_regions', 'root_scroll_refresh',
      'node', 'view_element', 'text_element', 'image_element', 'style', 'css_atom',
      'document', 'core', 'tree_nodes', 'tree_style', 'tree_scroll_mirror', 'tree_render',
      'view', 'text', 'image_node', 'layout', 'render', 'input', 'input_render',
      'virtual_keyboard', 'canvas_element', 'canvas_store', 'tree_events'
    ]),
    ...cpp(engine, ['canvas', 'image_store', 'bitmap_font', 'rasterized_font', 'touch_runtime']),
    // @geastack/elements — optional rich elements (engine still dispatches via NodeType until the behavior-table lands)
    ...cpp(join(elements, 'ui'), ['camera_element', 'virtual_list']),
    // @geastack/host — platform layer + services
    ...cpp(host, ['input', 'wifi', 'bluetooth', 'geolocation']),
    ...cpp(join(host, 'host'), [
      'timers', 'worker', 'input', 'device', 'display', 'fetch', 'media', 'rtc', 'websocket', 'http', 'wifi',
      'ble', 'apps', 'audio', 'audio_worklet', 'video', 'mjpeg', 'image', 'touch', 'imu', 'memory', 'camera', 'geolocation',
      'tile_loader'
    ]),
    // framework services (built on the platform abstraction — no target-specific deps)
    ...cpp(join(host, 'services'), [
      'app_state', 'battery_service', 'bluetooth_service', 'diagnostics',
      'diagnostics_connection', 'diagnostics_server', 'mirror', 'network_services',
      'runtime_log'
    ]),
    // @geastack/core — single-app shell: frame loop + perf + event dispatch
    ...cpp(core, ['app_frame_perf', 'runtime', 'events']),
    // geaos — installed-application platform integration
    ...cpp(geaos, ['app_manager', 'installed_apps'])
  ]
}

const queries = {
  'include-flags': includeFlags,
  'c-sources': cSources,
  'cxx-sources': cxxSources
}

const invokedDirectly = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)
if (invokedDirectly) {
  const query = queries[process.argv[2]]
  if (!query) {
    process.stderr.write(`usage: gea_sources.mjs ${Object.keys(queries).join('|')}\n`)
    process.exit(2)
  }
  try {
    process.stdout.write(`${query().join('\n')}\n`)
  } catch (error) {
    process.stderr.write(`${error.message}\n`)
    process.exit(1)
  }
}
