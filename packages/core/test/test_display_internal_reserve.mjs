// The display's internal-RAM reserve has to be a REAL call on the platform
// Memory API, never a weak hook. The first cut of this declared
//
//   extern "C" void *gea_platform_reserve_internal_dma(unsigned long) __attribute__((weak));
//
// in runtime.h and defined it in the esp32 target's services/memory.cpp. It
// compiled, the define reached runtime.cpp, and the reserve NEVER HAPPENED on
// any image: a weak *undefined* reference does not pull the defining member out
// of a static archive, so memory.cpp.obj stayed in libmain.a, the pointer linked
// to null and the `if (hook)` guard skipped the whole thing silently. Nothing in
// the log, no link error, and a pedal with a blank panel. A strong reference to
// Memory::reserveInternalDma extracts the member, so this stays a method.
//
// This is the framework half. Each target repo asserts its own implementation.
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'

const source = (rel) => readFileSync(new URL(`../${rel}`, import.meta.url), 'utf8')

const memoryHeader = source('include/memory.h')
const runtimeHeader = source('include/runtime.h')
const runtime = source('runtime.cpp')

assert.match(memoryHeader, /static void \*reserveInternalDma\(std::size_t bytes\);/)
assert.match(memoryHeader, /static void releaseInternalDma\(void \*reserve\);/)

assert.doesNotMatch(
  runtimeHeader,
  /gea_platform_reserve_internal_dma|gea_platform_release_internal_dma/,
  'the reserve must not come back as a weak hook -- a weak undefined reference links to null'
)
assert.match(runtimeHeader, /#ifndef GEA_EMBEDDED_DISPLAY_INTERNAL_RESERVE_BYTES\n#define GEA_EMBEDDED_DISPLAY_INTERNAL_RESERVE_BYTES 0/)

assert.match(
  runtime,
  /void \*displayReserve =\s*\n?\s*gea::platform::memory::Memory::reserveInternalDma\(GEA_EMBEDDED_DISPLAY_INTERNAL_RESERVE_BYTES\);/,
  'Runtime::boot has to take the reserve through the platform Memory API'
)
assert.doesNotMatch(runtime, /if \(displayReserve &&/, 'releaseInternalDma tolerates nullptr itself')

// Both bring-up paths (direct-canvas and the full runtime) release before
// Display::init(), or the reserve is still held when the panel needs it.
//
// Ordering, not adjacency. This used to match the two lines as one glued
// pattern and went stale the moment a comment and an `#if !GEA_EMBEDDED_NO_DISPLAY`
// were inserted between them -- the invariant still held, the regex did not.
const offsets = (needle) => {
  const out = []
  for (let i = runtime.indexOf(needle); i !== -1; i = runtime.indexOf(needle, i + 1)) out.push(i)
  return out
}
const releases = offsets('releaseDisplayReserves(displayReserve);')
const inits = offsets('gea::platform::display::Display::init()')
assert.equal(releases.length, 2, 'both bring-up paths must release the reserve')
assert.equal(inits.length, 2, 'there are two bring-up paths')
for (const [i, init] of inits.entries()) {
  assert.ok(releases[i] < init, `Display::init() #${i + 1} must come after its release`)
  assert.ok(
    releases.filter((r) => r < init).length === i + 1,
    `Display::init() #${i + 1} must be preceded by exactly its own release, not a later one`
  )
}

// An app whose boot hook has a mandatory phase and a greedy one takes the
// reserve itself, between them, and hands it over. Both reserves are then freed
// together, and holding a second one must not leak the first.
assert.match(runtimeHeader, /static void holdDisplayReserve\(void \*reserve\);/)
assert.match(runtime, /void Runtime::holdDisplayReserve\(void \*reserve\)/)
assert.match(
  runtime,
  /if \(g_appDisplayReserve == reserve\) return;\n\tgea::platform::memory::Memory::releaseInternalDma\(g_appDisplayReserve\);/,
  'a second hold must free the block it replaces'
)
assert.match(
  runtime,
  /void releaseDisplayReserves\(void \*runtimeReserve\)[\s\S]*releaseInternalDma\(runtimeReserve\);[\s\S]*releaseInternalDma\(g_appDisplayReserve\);[\s\S]*g_appDisplayReserve = nullptr;/,
  'the app reserve has to be freed with the runtime one and the handle cleared'
)

// The synchronous Wi-Fi loan must last through app/task allocation, but must
// not pin a running animated app to the two-row radio-startup floor forever.
const stagingRelease = runtime.indexOf('gea::platform::display::Display::reserveInternal(0);')
assert.ok(stagingRelease > runtime.indexOf('services::FrameScheduler::start(eventQueue);'),
  'release Wi-Fi staging only after startup allocations are owned')
assert.ok(stagingRelease > runtime.indexOf('services::AppRunner::initApplication(options.width, options.height)'),
  'the native application must allocate before display staging can regrow')
