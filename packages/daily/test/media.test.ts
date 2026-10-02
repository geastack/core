import assert from "node:assert/strict";
import test from "node:test";
import { DailyMedia } from "../src/media.ts";
import type { MediaSignaling } from "../src/media.ts";
import type { DailyCommand } from "../src/types.ts";

const fingerprint = new Array<string>(32).fill("AB").join(":");
const opus = {
  mimeType: "audio/opus",
  preferredPayloadType: 100,
  clockRate: 48000,
  channels: 2,
};
const h264 = {
  mimeType: "video/H264",
  preferredPayloadType: 107,
  clockRate: 90000,
  parameters: { "profile-level-id": "42e01f", "packetization-mode": 1 },
};
const transport = {
  id: "transport",
  iceParameters: {
    usernameFragment: "ufrag",
    password: "password",
    iceLite: true,
  },
  iceCandidates: [],
  dtlsParameters: {
    role: "auto",
    fingerprints: [{ algorithm: "sha-256", value: fingerprint }],
  },
};
const localSdp = [
  "v=0",
  `a=fingerprint:sha-256 ${fingerprint}`,
  "a=setup:active",
  "m=audio 9 UDP/TLS/RTP/SAVPF 111",
  "a=mid:0",
  "a=rtpmap:111 opus/48000/2",
  "a=ssrc:123 cname:mic",
].join("\r\n");

class Peer {
  closed = false;
  ontrack: RTCPeerConnection["ontrack"] = null;
  onconnectionstatechange: RTCPeerConnection["onconnectionstatechange"] = null;
  remote: RTCSessionDescriptionInit | null = null;
  local: RTCSessionDescriptionInit | null = null;
  transceivers: string[] = [];

  addTransceiver(track: string | MediaStreamTrack): void {
    this.transceivers.push(typeof track === "string" ? track : track.kind);
  }
  async setRemoteDescription(
    description: RTCSessionDescriptionInit,
  ): Promise<void> {
    this.remote = description;
  }
  async setLocalDescription(
    description: RTCSessionDescriptionInit,
  ): Promise<void> {
    this.local = description;
  }
  async createAnswer(): Promise<RTCSessionDescriptionInit> {
    return { type: "answer", sdp: localSdp };
  }
  async createOffer(): Promise<RTCSessionDescriptionInit> {
    return { type: "offer", sdp: localSdp };
  }
  close(): void {
    this.closed = true;
  }
}

class Signaling implements MediaSignaling {
  calls: { command: DailyCommand; fields: object }[] = [];
  audio = true;

  async request(
    command: DailyCommand,
    fields: object,
  ): Promise<Record<string, unknown>> {
    this.calls.push({ command, fields });
    if (command === "create-transport") return { transportOptions: transport };
    if (command === "send-track") return { producerInfo: { id: "mic" } };
    if (command === "recv-track") {
      const video = "mediaTag" in fields && fields.mediaTag === "cam-video";
      const codec = video ? h264 : opus;
      return {
        consumerParameters: {
          id: video ? "video" : "audio",
          producerId: "avatar",
          kind: video ? "video" : "audio",
          rtpParameters: {
            codecs: [{ ...codec, payloadType: codec.preferredPayloadType }],
            encodings: [{ ssrc: video ? 10 : 11 }],
            rtcp: { cname: "avatar" },
          },
        },
      };
    }
    return {};
  }

  setMicrophoneEnabled(enabled: boolean): void {
    this.audio = enabled;
  }
}

function setup(
  signaling: MediaSignaling = new Signaling(),
  receiveVideo = true,
) {
  const peers: Peer[] = [];
  const errors: Error[] = [];
  const media = new DailyMedia({
    receiveVideo,
    signaling,
    createPeer: () => {
      const peer = new Peer();
      peers.push(peer);
      // The fake implements only the native media surface exercised here.
      return peer as unknown as RTCPeerConnection;
    },
    onTrack: () => {},
    onError: (error) => errors.push(error),
  });
  return { media, peers, errors };
}

test("an external video sink negotiates only avatar audio, ignoring the local microphone", async () => {
  const signaling = new Signaling();
  const { media, peers, errors } = setup(signaling, false);
  await media.start({ codecs: [opus, h264] }, null);
  media.updateTracks([{ peerId: "local", mediaTag: "cam-audio" }]);
  await settle();
  assert.equal(peers.length, 0);
  media.updateTracks([
    { peerId: "local", mediaTag: "cam-audio" },
    { peerId: "avatar", mediaTag: "cam-audio" },
    { peerId: "avatar", mediaTag: "cam-video" },
  ]);
  await settle();
  assert.deepEqual(errors, []);
  assert.deepEqual(peers[0]?.transceivers, ["audio"]);
  assert.doesNotMatch(peers[0]?.remote?.sdp ?? "", /m=video/);
  const requests = signaling.calls.filter(
    (call) => call.command === "recv-track",
  );
  assert.equal(requests.length, 1);
  assert.equal(
    "mediaTag" in requests[0].fields && requests[0].fields.mediaTag,
    "cam-audio",
  );
  assert.equal(
    "mediaPeerId" in requests[0].fields && requests[0].fields.mediaPeerId,
    "avatar",
  );
  assert.equal(
    signaling.calls.filter((call) => call.command === "resume-consumer").length,
    1,
  );
  media.close();
});

async function settle(): Promise<void> {
  for (let i = 0; i < 30; ++i) await Promise.resolve();
}

test("announcements received before SFU setup negotiate audio and video together", async () => {
  const signaling = new Signaling();
  const { media, peers, errors } = setup(signaling);
  media.updateTracks([
    { peerId: "avatar", mediaTag: "cam-audio" },
    { peerId: "avatar", mediaTag: "cam-video" },
  ]);
  await media.start({ codecs: [opus, h264] }, null);
  await settle();
  assert.deepEqual(errors, []);
  assert.deepEqual(peers[0]?.transceivers, ["audio", "video"]);
  assert.match(peers[0]?.remote?.sdp ?? "", /a=group:BUNDLE 0 1/);
  assert.equal(
    signaling.calls.filter((call) => call.command === "resume-consumer").length,
    2,
  );
  const count = signaling.calls.length;
  media.updateTracks([{ peerId: "avatar", mediaTag: "cam-video" }]);
  assert.equal(
    signaling.calls.length,
    count,
    "temporary track announcements must not stop ongoing playback",
  );
  media.close();
  assert.equal(peers[0]?.closed, true);
});

test("an earlier microphone-only peer does not prevent subscribing to the avatar", async () => {
  const signaling = new Signaling();
  const { media, peers, errors } = setup(signaling);
  await media.start({ codecs: [opus, h264] }, null);
  media.updateTracks([{ peerId: "local", mediaTag: "cam-audio" }]);
  await settle();
  assert.equal(peers.length, 0);
  media.updateTracks([
    { peerId: "local", mediaTag: "cam-audio" },
    { peerId: "avatar", mediaTag: "cam-audio" },
    { peerId: "avatar", mediaTag: "cam-video" },
  ]);
  await settle();
  assert.deepEqual(errors, []);
  assert.equal(peers.length, 1);
  const requests = signaling.calls.filter(
    (call) => call.command === "recv-track",
  );
  assert.equal(requests.length, 2);
  for (const { fields } of requests) {
    assert.equal("mediaPeerId" in fields && fields.mediaPeerId, "avatar");
  }
  media.close();
});

test("video resume does not wait for the audio resume acknowledgement", async () => {
  class DelayedAudioSignaling extends Signaling {
    release!: (value: Record<string, unknown>) => void;

    override async request(
      command: DailyCommand,
      fields: object,
    ): Promise<Record<string, unknown>> {
      const result = super.request(command, fields);
      if (
        command === "resume-consumer" &&
        "consumerId" in fields &&
        fields.consumerId === "audio"
      ) {
        return new Promise((resolve) => {
          this.release = resolve;
        });
      }
      return result;
    }
  }
  const signaling = new DelayedAudioSignaling();
  const { media, errors } = setup(signaling);
  await media.start({ codecs: [opus, h264] }, null);
  media.updateTracks([
    { peerId: "avatar", mediaTag: "cam-audio" },
    { peerId: "avatar", mediaTag: "cam-video" },
  ]);
  await settle();
  try {
    assert.deepEqual(
      signaling.calls
        .filter((call) => call.command === "resume-consumer")
        .map((call) => call.fields),
      [{ consumerId: "audio" }, { consumerId: "video" }],
    );
  } finally {
    signaling.release({});
    await settle();
    media.close();
  }
  assert.deepEqual(errors, []);
});

test("microphone muting is immediate and does not close the receive transport", async () => {
  const signaling = new Signaling();
  const { media, peers } = setup(signaling);
  const microphone = { kind: "audio", enabled: true } as MediaStreamTrack;
  await media.start({ codecs: [opus, h264] }, microphone);
  media.setMicrophoneEnabled(false);
  assert.equal(microphone.enabled, false);
  assert.equal(signaling.audio, false);
  assert.equal(signaling.calls.at(-1)?.command, "pause-producer");
  assert.equal(peers[0]?.closed, false);
  media.close();
  assert.equal(peers[0]?.closed, true);
});

test("close during transport creation cannot start a late peer", async () => {
  let release!: (value: Record<string, unknown>) => void;
  const pending = new Promise<Record<string, unknown>>((resolve) => {
    release = resolve;
  });
  const { media, peers } = setup({
    request: () => pending,
    setMicrophoneEnabled: () => {},
  });
  const start = media.start({ codecs: [opus, h264] }, null);
  media.close();
  release({ transportOptions: transport });
  await assert.rejects(start, /closed/);
  assert.equal(peers.length, 0);
});
