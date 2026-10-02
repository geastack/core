import { isRecord } from './types.ts'
import type { DailyClientInfo, DailyRoom, DailyRoomAddress } from './types.ts'

/** Do not forward room tokens to arbitrary hosts or accept a URL path as a room. */
export function parseRoomAddress(url: string): DailyRoomAddress {
  const match = /^https:\/\/([a-z0-9-]+)\.daily\.co\/([a-zA-Z0-9_-]+)\/?$/.exec(url)

  if (!match || !match[1] || !match[2]) {
    throw new Error('Expected an HTTPS Daily room URL.')
  }

  return { domain: match[1], room: match[2] }
}

export function roomLookupRequest(
  address: DailyRoomAddress,
  token: string,
  sessionId: string,
  client: DailyClientInfo,
): { url: string; body: string; contentType: string } {
  return {
    url: `https://gs.daily.co/rooms/check/${encodeURIComponent(address.domain)}/${encodeURIComponent(address.room)}`,
    body: `aboutClient=${encodeURIComponent(JSON.stringify(client))}&sessionId=${encodeURIComponent(sessionId)}&joinToken=${encodeURIComponent(token)}`,
    contentType: 'application/x-www-form-urlencoded',
  }
}

export function parseRoomLookup(address: DailyRoomAddress, value: unknown): DailyRoom {
  if (!isRecord(value) || value.error || value.needToRequest === true) {
    // Never include an upstream response in an error: it can contain join tokens.
    throw new Error('Daily did not authorize the room connection.')
  }

  const worker = value.worker

  if (
    !isRecord(worker) ||
    typeof worker.workerId !== 'string' ||
    typeof worker.wssUri !== 'string' ||
    !worker.wssUri.startsWith('wss://') ||
    typeof value.sigAuthz !== 'string' ||
    !value.sigAuthz
  ) {
    throw new Error('Daily room discovery returned incomplete signaling details.')
  }

  return {
    address,
    worker: { workerId: worker.workerId, wssUri: worker.wssUri },
    sigAuthz: value.sigAuthz,
    iceConfig: value.iceConfig,
  }
}
