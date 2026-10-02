import { token } from './rtp.ts'
import type { Consumer, DtlsParameters, RemoteTransport, RtpCodec, RtpParameters } from './rtp.ts'

function codecLines(codec: RtpCodec): string[] {
  const name = codec.mimeType.split('/')[1]
  if (!name) throw new Error('Invalid RTP codec name.')
  const suffix = codec.channels ? `/${codec.channels}` : ''
  const lines = [`a=rtpmap:${codec.payloadType} ${name}/${codec.clockRate}${suffix}`]
  const parameters = Object.entries(codec.parameters).map(([key, value]) => `${key}=${value}`)
  if (parameters.length) lines.push(`a=fmtp:${codec.payloadType} ${parameters.join(';')}`)
  for (const feedback of codec.rtcpFeedback) {
    lines.push(
      `a=rtcp-fb:${codec.payloadType} ${feedback.type}${feedback.parameter ? ` ${feedback.parameter}` : ''}`,
    )
  }
  return lines
}

function transportLines(transport: RemoteTransport, offer: boolean): string[] {
  const fingerprint = transport.dtlsParameters.fingerprints.find(
    (value) => value.algorithm === 'sha-256',
  )
  if (!fingerprint) throw new Error('The SFU did not provide a SHA-256 fingerprint.')
  const lines = [
    `a=ice-ufrag:${transport.iceParameters.usernameFragment}`,
    `a=ice-pwd:${transport.iceParameters.password}`,
    `a=fingerprint:${fingerprint.algorithm} ${fingerprint.value}`,
    `a=setup:${offer ? 'actpass' : 'passive'}`,
  ]
  for (const candidate of transport.iceCandidates) {
    lines.push(
      `a=candidate:${candidate.foundation} 1 ${candidate.protocol} ${candidate.priority} ${candidate.ip} ${candidate.port} typ ${candidate.type}${candidate.tcpType ? ` tcptype ${candidate.tcpType}` : ''}`,
    )
  }
  lines.push('a=end-of-candidates')
  return lines
}

/** One receive transport carries the avatar's audio and video in one BUNDLE. */
export function receiveOffer(
  transport: RemoteTransport,
  consumers: readonly Consumer[],
  version: number,
): string {
  if (!consumers.length) throw new Error('Cannot negotiate an empty receive transport.')
  const mids = consumers.map((consumer, index) => String(index))
  const lines = [
    'v=0',
    `o=- 10000 ${version} IN IP4 0.0.0.0`,
    's=-',
    't=0 0',
    `a=group:BUNDLE ${mids.join(' ')}`,
    'a=msid-semantic: WMS *',
  ]
  if (transport.iceParameters.iceLite) lines.push('a=ice-lite')
  consumers.forEach((consumer, index) => {
    const rtp = consumer.rtpParameters
    lines.push(
      `m=${consumer.kind} 7 UDP/TLS/RTP/SAVPF ${rtp.codecs.map((codec) => codec.payloadType).join(' ')}`,
      'c=IN IP4 0.0.0.0',
      `a=mid:${mids[index]}`,
      'a=sendonly',
      ...transportLines(transport, true),
      'a=rtcp-mux',
    )
    if (rtp.rtcp.reducedSize) lines.push('a=rtcp-rsize')
    for (const codec of rtp.codecs) lines.push(...codecLines(codec))
    for (const extension of rtp.headerExtensions)
      lines.push(`a=extmap:${extension.id} ${extension.uri}`)
    lines.push(`a=msid:daily ${consumer.id}`)
    for (const encoding of rtp.encodings) {
      lines.push(
        `a=ssrc:${encoding.ssrc} cname:${rtp.rtcp.cname}`,
        `a=ssrc:${encoding.ssrc} msid:daily ${consumer.id}`,
      )
    }
  })
  return `${lines.join('\r\n')}\r\n`
}

export function localDtlsParameters(sdp: string): DtlsParameters {
  const fingerprints: DtlsParameters['fingerprints'] = []
  let role = 'client'
  for (const line of sdp.split(/\r?\n/)) {
    if (line.startsWith('a=fingerprint:sha-256 ')) {
      const value = token(line.slice('a=fingerprint:sha-256 '.length))
      if (!fingerprints.some((item) => item.value === value))
        fingerprints.push({ algorithm: 'sha-256', value })
    }
    if (line === 'a=setup:passive') role = 'server'
  }
  if (!fingerprints.length) throw new Error('The local peer did not provide a DTLS fingerprint.')
  return { role, fingerprints }
}

/** Extract the actual microphone SSRC/Opus mapping chosen by the native peer. */
export function microphoneParameters(sdp: string): RtpParameters {
  let audio = false
  let mid = ''
  let cname = ''
  let ssrc = 0
  let payload = -1
  const parameters: Record<string, string | number> = {}
  const lines = sdp.split(/\r?\n/)
  for (const line of lines) {
    if (line.startsWith('m=')) audio = line.startsWith('m=audio ')
    if (!audio) continue
    if (line.startsWith('a=mid:')) mid = token(line.slice(6))
    const codec = /^a=rtpmap:(\d+) opus\/48000\/2$/i.exec(line)
    if (codec) payload = Number(codec[1])
    const source = /^a=ssrc:(\d+) cname:(\S+)$/.exec(line)
    if (source) {
      ssrc = Number(source[1])
      cname = token(source[2])
    }
  }
  if (payload < 0 || payload > 127 || !ssrc || !cname || !mid)
    throw new Error('The local microphone SDP is incomplete.')
  for (const line of lines) {
    if (line.startsWith(`a=fmtp:${payload} `)) {
      for (const entry of line.slice(line.indexOf(' ') + 1).split(';')) {
        const split = entry.trim().split('=')
        if (split.length === 2) parameters[token(split[0])] = token(split[1])
      }
    }
  }
  return {
    mid,
    codecs: [
      {
        mimeType: 'audio/opus',
        payloadType: payload,
        clockRate: 48000,
        channels: 2,
        parameters,
        rtcpFeedback: [],
      },
    ],
    headerExtensions: [],
    encodings: [{ ssrc }],
    rtcp: { cname, reducedSize: true, mux: true },
  }
}

export function microphoneAnswer(transport: RemoteTransport, rtp: RtpParameters): string {
  const lines = ['v=0', 'o=- 10001 1 IN IP4 0.0.0.0', 's=-', 't=0 0', `a=group:BUNDLE ${rtp.mid}`]
  if (transport.iceParameters.iceLite) lines.push('a=ice-lite')
  lines.push(
    `m=audio 7 UDP/TLS/RTP/SAVPF ${rtp.codecs.map((codec) => codec.payloadType).join(' ')}`,
    'c=IN IP4 0.0.0.0',
    `a=mid:${rtp.mid}`,
    'a=recvonly',
    ...transportLines(transport, false),
    'a=rtcp-mux',
    'a=rtcp-rsize',
  )
  for (const codec of rtp.codecs) lines.push(...codecLines(codec))
  return `${lines.join('\r\n')}\r\n`
}
