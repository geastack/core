/** Native compiler/runtime probe. Uses the same TypeScript transport as the app. */
import { DailySignaling } from '../src/signaling.ts'
import type { DailySocket } from '../src/types.ts'
import type { DailyTrack } from '../src/types.ts'

let lastSent = ''
let closed = false
let timer = 0
let tracks: readonly DailyTrack[] = []
const socket: DailySocket = {
  onopen: null,
  onmessage: null,
  onerror: null,
  onclose: null,
  send(data: string): void {
    lastSent = data
  },
  close(): void {
    closed = true
  },
}
const signaling = new DailySignaling({
  socket: () => socket,
  clock: {
    now: () => 100,
    later: () => ++timer,
    cancel: () => {},
  },
  sessionId: 'native-client',
  meetingSessionId: 'native-meeting',
  userName: 'Gea',
  client: { library: 'gea', version: '0.0.1' },
  onTracks: (value) => {
    tracks = value
  },
})

async function main(): Promise<void> {
  const joining = signaling.connect({
    address: { domain: 'test', room: 'room' },
    worker: { workerId: 'worker', wssUri: 'wss://worker.daily.co' },
    sigAuthz: 'test-authorization',
    iceConfig: null,
  })

  socket.onopen?.()
  if (!lastSent.includes('join-for-sig')) {
    throw new Error('Join was not sent.')
  }

  socket.onmessage?.({ data: '{"msgStr":"sig-ack"}' })
  await joining
  const request = signaling.request('create-transport', { direction: 'recv' })
  if (!lastSent.includes('"direction":"recv"') || !lastSent.includes('"_sendTs":"100.1"')) {
    throw new Error('RPC fields were lost before transmission.')
  }

  socket.onmessage?.({
    data: '{"tag":"soup","msgData":{"_kTs":"100.1","id":"receive"}}',
  })
  const response = await request

  if (response.id !== 'receive') {
    throw new Error('RPC did not resolve the expected response.')
  }

  socket.onmessage?.({
    data: '{"tag":"soup","msgData":{"tracks":[["avatar","cam-audio"],["avatar","cam-video"]]}}',
  })
  if (tracks.length !== 2 || tracks[1]?.mediaTag !== 'cam-video') {
    throw new Error('Initial audio/video track announcements were lost.')
  }
  socket.onmessage?.({
    data: '{"tag":"soup","msgData":{"ptracks":{"avatar":["cam-audio","cam-video",42]}}}',
  })
  if (
    tracks.length !== 2 ||
    tracks[0]?.peerId !== 'avatar' ||
    tracks[1]?.mediaTag !== 'cam-video'
  ) {
    throw new Error('Incremental audio/video track announcements were lost.')
  }
  console.log('native Daily track announcements passed')

  signaling.sendAppMessage({ event: 'force-end' })
  if (!lastSent.includes('force-end')) {
    throw new Error('Stop was not sent.')
  }

  signaling.close()
  if (!closed) {
    throw new Error('Socket was not closed.')
  }

  console.log('native Daily signaling passed')
}

void main().catch((error: unknown) => {
  console.log('native Daily signaling failed:', String(error))
})
