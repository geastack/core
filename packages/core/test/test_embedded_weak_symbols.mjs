import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'

const entry = readFileSync(new URL('../gea_app_entry.cpp', import.meta.url), 'utf8')

assert.doesNotMatch(entry, /weak_import/, 'embedded GCC does not support the Mach-O weak_import attribute')
// The deferral hooks are weak through GEA_PROGRAM_HOOK, which only a target
// that links their defaults (GEA_LINKED_HOOK_DEFAULTS) turns into plain references.
assert.match(entry, /#else\n#define GEA_PROGRAM_HOOK __attribute__\(\(weak\)\)\n#endif/)
assert.match(entry, /gea_cycle_collection_defer_begin\(\) GEA_PROGRAM_HOOK;/)
assert.match(entry, /gea_cycle_collection_defer_end\(\) GEA_PROGRAM_HOOK;/)
assert.match(entry, /gea_embedded_assets_prepare\(\) GEA_PROGRAM_HOOK;/)

const initBody = entry.slice(entry.indexOf('void Application::init'), entry.indexOf('namespace {', entry.indexOf('void Application::init')))
const frameBody = entry.slice(entry.indexOf('void Application::frame'), entry.indexOf('void Application::toggleSettings'))
assert.match(initBody, /FrameCycleCollectionDeferral deferCycleCollection;/)
assert.match(frameBody, /FrameCycleCollectionDeferral deferCycleCollection;/)
