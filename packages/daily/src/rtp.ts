import { isRecord } from "./types.ts";

export interface RtcpFeedback {
  type: string;
  parameter: string;
}

export interface RtpCodec {
  mimeType: string;
  payloadType: number;
  clockRate: number;
  channels?: number;
  parameters: Record<string, string | number>;
  rtcpFeedback: RtcpFeedback[];
}

export interface RtpEncoding {
  ssrc: number;
}

export interface RtpParameters {
  mid?: string;
  codecs: RtpCodec[];
  headerExtensions: { uri: string; id: number }[];
  encodings: RtpEncoding[];
  rtcp: { cname: string; reducedSize: boolean; mux: boolean };
}

export interface RtpCapabilities {
  codecs: {
    kind: string;
    mimeType: string;
    preferredPayloadType: number;
    clockRate: number;
    channels?: number;
    parameters: Record<string, string | number>;
    rtcpFeedback: RtcpFeedback[];
  }[];
  headerExtensions: { kind: string; uri: string; preferredId: number }[];
}

export interface DtlsParameters {
  role: string;
  fingerprints: { algorithm: string; value: string }[];
}

export interface RemoteTransport {
  id: string;
  iceParameters: {
    usernameFragment: string;
    password: string;
    iceLite: boolean;
  };
  iceCandidates: {
    foundation: string;
    priority: number;
    ip: string;
    protocol: string;
    port: number;
    type: string;
    tcpType?: string;
  }[];
  dtlsParameters: DtlsParameters;
}

export interface Consumer {
  id: string;
  producerId: string;
  kind: "audio" | "video";
  rtpParameters: RtpParameters;
}

function record(value: unknown): Record<string, unknown> {
  if (!isRecord(value)) throw new Error("Invalid SFU media parameters.");
  return value;
}

function list(value: unknown): unknown[] {
  if (!Array.isArray(value)) throw new Error("Invalid SFU media list.");
  return value;
}

/** SDP tokens must never contain injected lines or whitespace. */
export function token(value: unknown): string {
  if (typeof value !== "string" || !/^[!-~]+$/.test(value)) {
    throw new Error("Invalid SFU media token.");
  }
  return value;
}

function integer(value: unknown, max = 4294967295): number {
  if (
    typeof value !== "number" ||
    !Number.isInteger(value) ||
    value < 0 ||
    value > max
  ) {
    throw new Error("Invalid SFU media number.");
  }
  return value;
}

function codecParameters(value: unknown): Record<string, string | number> {
  const result: Record<string, string | number> = {};
  for (const [name, entry] of Object.entries(record(value ?? {}))) {
    if (!/^[A-Za-z0-9_-]+$/.test(name))
      throw new Error("Invalid codec parameter name.");
    if (typeof entry === "number" && Number.isFinite(entry))
      result[name] = entry;
    else {
      const text = token(entry);
      if (text.includes(";")) throw new Error("Invalid codec parameter value.");
      result[name] = text;
    }
  }
  return result;
}

function codec(value: unknown, capability = false): RtpCodec {
  const item = record(value);
  const feedback: RtcpFeedback[] = [];
  for (const entry of list(item.rtcpFeedback ?? [])) {
    const field = record(entry);
    feedback.push({
      type: token(field.type),
      parameter: field.parameter ? token(field.parameter) : "",
    });
  }
  return {
    mimeType: token(item.mimeType),
    payloadType: integer(
      capability ? item.preferredPayloadType : item.payloadType,
      127,
    ),
    clockRate: integer(item.clockRate, 192000),
    ...(item.channels === undefined
      ? {}
      : { channels: integer(item.channels, 2) }),
    parameters: codecParameters(item.parameters),
    rtcpFeedback: feedback,
  };
}

/** Receive payload IDs belong to the router, not to a browser's local offer. */
export function receiveCapabilities(value: unknown): RtpCapabilities {
  const capabilities = record(value);
  const codecs: RtpCapabilities["codecs"] = [];
  for (const entry of list(capabilities.codecs)) {
    const item = codec(entry, true);
    const mime = item.mimeType.toLowerCase();
    const opus =
      mime === "audio/opus" && item.clockRate === 48000 && item.channels === 2;
    const profile = String(item.parameters["profile-level-id"] ?? "");
    const h264 =
      mime === "video/h264" &&
      item.clockRate === 90000 &&
      item.parameters["packetization-mode"] === 1 &&
      /^42[0-9a-f]{4}$/i.test(profile);
    const vp8 =
      mime === "video/vp8" &&
      item.clockRate === 90000 &&
      item.rtcpFeedback.some(
        (feedback) => feedback.type === "nack" && feedback.parameter === "pli",
      );
    if (!opus && !h264 && !vp8) continue;
    if (codecs.some((existing) => existing.mimeType.toLowerCase() === mime))
      continue;
    codecs.push({
      kind: opus ? "audio" : "video",
      mimeType: item.mimeType,
      preferredPayloadType: item.payloadType,
      clockRate: item.clockRate,
      ...(item.channels === undefined ? {} : { channels: item.channels }),
      parameters: item.parameters,
      // The VP8 receiver recovers loss via PLI; H264 uses esp_peer's NACK/PLI.
      // Neither path advertises RTX or transport-cc without its native support.
      rtcpFeedback: item.rtcpFeedback.filter(
        (feedback) =>
          feedback.type === "nack" && (!vp8 || feedback.parameter === "pli"),
      ),
    });
  }
  if (
    !codecs.some((item) => item.kind === "audio") ||
    !codecs.some((item) => item.kind === "video")
  )
    throw new Error("The SFU does not offer Opus and VP8 or baseline H.264.");
  return { codecs, headerExtensions: [] };
}

export function parseTransport(value: unknown): RemoteTransport {
  const item = record(value);
  const ice = record(item.iceParameters);
  const dtls = record(item.dtlsParameters);
  return {
    id: token(item.id),
    iceParameters: {
      usernameFragment: token(ice.usernameFragment),
      password: token(ice.password),
      iceLite: ice.iceLite === true,
    },
    iceCandidates: list(item.iceCandidates).map((entry) => {
      const candidate = record(entry);
      const protocol = token(candidate.protocol);
      const type = token(candidate.type);
      if (protocol !== "udp" && protocol !== "tcp")
        throw new Error("Unsupported ICE protocol.");
      if (!["host", "srflx", "prflx", "relay"].includes(type))
        throw new Error("Invalid ICE candidate type.");
      return {
        foundation: token(candidate.foundation),
        priority: integer(candidate.priority),
        ip: token(candidate.ip ?? candidate.address),
        protocol,
        port: integer(candidate.port, 65535),
        type,
        ...(candidate.tcpType === undefined
          ? {}
          : { tcpType: token(candidate.tcpType) }),
      };
    }),
    dtlsParameters: {
      role: token(dtls.role ?? "auto"),
      fingerprints: list(dtls.fingerprints).map((entry) => {
        const fingerprint = record(entry);
        const algorithm = token(fingerprint.algorithm);
        const value = token(fingerprint.value);
        if (
          !/^sha-(1|224|256|384|512)$/.test(algorithm) ||
          !/^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2})+$/.test(value)
        ) {
          throw new Error("Unsupported DTLS fingerprint.");
        }
        return { algorithm, value };
      }),
    },
  };
}

export function parseConsumer(value: unknown): Consumer {
  const item = record(value);
  if (item.kind !== "audio" && item.kind !== "video")
    throw new Error("Invalid consumer media kind.");
  const rtp = record(item.rtpParameters);
  const rtcp = record(rtp.rtcp);
  const codecs = list(rtp.codecs).map((entry) => codec(entry));
  const encodings = list(rtp.encodings).map((entry) => ({
    ssrc: integer(record(entry).ssrc),
  }));
  if (codecs.length !== 1 || encodings.length !== 1)
    throw new Error("Unsupported consumer encoding.");
  return {
    id: token(item.id),
    producerId: token(item.producerId),
    kind: item.kind,
    rtpParameters: {
      codecs,
      headerExtensions: list(rtp.headerExtensions ?? []).map((entry) => {
        const extension = record(entry);
        return { uri: token(extension.uri), id: integer(extension.id, 255) };
      }),
      encodings,
      rtcp: {
        cname: token(rtcp.cname),
        reducedSize: rtcp.reducedSize === true,
        mux: true,
      },
    },
  };
}
