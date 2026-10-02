export interface DailyClientInfo {
  library: string
  version: string
}

export interface DailyRoomAddress {
  domain: string
  room: string
}

export interface DailyWorker {
  workerId: string
  wssUri: string
}

export interface DailyRoom {
  address: DailyRoomAddress
  worker: DailyWorker
  sigAuthz: string
  /** Untrusted optional ICE configuration; the RTC adapter validates it. */
  iceConfig: unknown
}

/** Small transport boundary, implemented by either Gea or the browser. */
export interface DailySocket {
  onopen: (() => void) | null
  onmessage: ((event: { data: string }) => void) | null
  onerror: (() => void) | null
  onclose: (() => void) | null
  send(data: string): void
  close(): void
}

export interface DailyClock {
  now(): number
  later(callback: () => void, milliseconds: number): number
  cancel(timer: number): void
}

export interface DailyTrack {
  peerId: string
  mediaTag: string
}

export interface DailyPresence {
  id: string
  disconnected: boolean
  name: string
}

export interface DailySignalingOptions {
  socket(url: string): DailySocket
  clock: DailyClock
  sessionId: string
  meetingSessionId: string
  userName: string
  client: DailyClientInfo
  requestTimeoutMs?: number
  onAppMessage?(from: string, message: unknown): void
  onPresence?(presence: DailyPresence): void
  onTracks?(tracks: readonly DailyTrack[]): void
  onConsumerClosed?(id: string): void
  onError?(error: Error): void
}

export type DailyCommand =
  | 'join-as-new-peer'
  | 'create-transport'
  | 'connect-transport'
  | 'restart-ice'
  | 'send-track'
  | 'recv-track'
  | 'pause-consumer'
  | 'resume-consumer'
  | 'close-consumer'
  | 'pause-producer'
  | 'resume-producer'
  | 'close-producer'
  | 'close-connection'
  | 'set-consumer-layers'

export function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
}
