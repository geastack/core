import assert from 'node:assert/strict'
import test from 'node:test'

import { createGeaHostShims } from '../dist/host-shims.js'

// A listener parameter declared as any event interface index.d.ts publishes is
// handed the engine's one event struct. `Event` is declared by `onScroll` and by
// `EventTarget.addEventListener(type, listener: (event: Event) => void)`; without
// a carrier it lowered to a synthesized record the runtime listener adapter
// cannot build from a PointerEvent, and the app aborted on the first scroll.
test('every declared event parameter type lowers to the engine event, including Event', () => {
  const definitions = createGeaHostShims()
  for (const name of ['Event', 'PointerEvent', 'TouchEvent', 'RotaryEvent', 'InputEvent', 'KeyEvent']) {
    assert.equal(definitions.nativeTypes?.[name], 'gea::framework::events::PointerEvent', `${name} must lower to the engine event`)
  }
  for (const member of ['type', 'target', 'currentTarget']) {
    const rows = definitions.nativeMemberPropertyGetters?.[member] ?? []
    assert.ok(rows.some(row => row.receiverTypes?.includes('Event')), `Event.${member} must be a native member read`)
  }
  for (const method of ['preventDefault', 'stopPropagation']) {
    const rows = definitions.nativeMemberMethods?.[method] ?? []
    assert.ok(rows.some(row => row.receiverTypes?.includes('Event')), `Event.${method}() must be a native member call`)
  }
})

test('geolocation snapshot operations publish their non-throwing physical contract', () => {
  const definitions = createGeaHostShims()
  for (const namespace of ['Geolocation', 'geolocation']) {
    const methods = definitions.nativeNamespaceMethods?.[namespace]
    assert.equal(methods?.currentPosition?.noThrow, true, `${namespace}.currentPosition must be sealed noThrow`)
    assert.equal(methods?.coords?.noThrow, true, `${namespace}.coords must be sealed noThrow`)
  }
})
