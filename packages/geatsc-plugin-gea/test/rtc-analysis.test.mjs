import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import test from 'node:test'
import { analyzeSourceHostBindings } from '../dist/analyze.js'

test('RTC and microphone globals retain networking and audio capabilities', (t) => {
  const file = '/virtual-rtc/index.js'
  t.mock.method(fs, 'existsSync', p => p === file)
  t.mock.method(fs, 'statSync', () => ({ isFile: () => true }))
  t.mock.method(fs, 'readFileSync', () => `
    const peer = new RTCPeerConnection({});
    const mic = navigator.mediaDevices.getUserMedia({ audio: true });
  `)
  assert.deepEqual(analyzeSourceHostBindings(file).bindings, ['audio', 'rtc'])
})

test('installed LiveKit source contributes the app audio and network requirements', (t) => {
  const entry = path.resolve(import.meta.dirname, '../../../../livekit-agent/conversation.js')
  // This integration fixture uses the actual npm SDK when installed locally.
  if (!fs.existsSync(entry)) { t.skip('LiveKit example is not installed beside core'); return }
  const result = analyzeSourceHostBindings(entry)
  assert.ok(result.bindings.includes('audio'), JSON.stringify(result.bindings))
  assert.ok(result.bindings.includes('rtc'), JSON.stringify(result.bindings))
  assert.ok(result.bindings.includes('websocket'), JSON.stringify(result.bindings))
})
