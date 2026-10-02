import type { DailyCommand, DailyTrack } from "./types.ts";
import { parseConsumer, parseTransport, receiveCapabilities } from "./rtp.ts";
import type { Consumer, RemoteTransport, RtpCapabilities } from "./rtp.ts";
import {
  localDtlsParameters,
  microphoneAnswer,
  microphoneParameters,
  receiveOffer,
} from "./sdp.ts";

export interface MediaSignaling {
  request(
    command: DailyCommand,
    fields: object,
  ): Promise<Record<string, unknown>>;
  setMicrophoneEnabled(enabled: boolean): void;
}

export interface DailyMediaOptions {
  /** Keep avatar selection, but omit video decoding for an external video sink. */
  receiveVideo?: boolean;
  signaling: MediaSignaling;
  createPeer(): RTCPeerConnection;
  onTrack(peerId: string, track: MediaStreamTrack): void;
  onError(error: Error): void;
}

/** Media uses native WebRTC. Daily's socket carries only signaling. */
export class DailyMedia {
  private sender: RTCPeerConnection | null = null;
  private receiver: RTCPeerConnection | null = null;
  private microphone: MediaStreamTrack | null = null;
  private producerId = "";
  private capabilities: RtpCapabilities | null = null;
  private transport: RemoteTransport | null = null;
  private pendingTracks: readonly DailyTrack[] = [];
  private consumers: Consumer[] = [];
  private receiving: Promise<void> | null = null;
  private closed = false;

  constructor(private readonly options: DailyMediaOptions) {}

  async start(
    routerCapabilities: unknown,
    microphone: MediaStreamTrack | null,
  ): Promise<void> {
    this.assertOpen();
    this.capabilities = receiveCapabilities(routerCapabilities);
    const receive = await this.options.signaling.request("create-transport", {
      direction: "recv",
    });
    this.assertOpen();
    this.transport = parseTransport(receive.transportOptions);
    this.updateTracks(this.pendingTracks);
    if (microphone) await this.publish(microphone);
    console.info("[Daily] Media startup completed");
  }

  updateTracks(tracks: readonly DailyTrack[]): void {
    this.pendingTracks = tracks;
    console.info(
      "[Daily] Track update",
      tracks.map((track) => track.mediaTag).join(","),
      "transport",
      this.transport !== null,
      "capabilities",
      this.capabilities !== null,
      "receiving",
      this.receiving !== null,
      "receiver",
      this.receiver !== null,
      "closed",
      this.closed,
    );
    if (
      this.closed ||
      !this.transport ||
      !this.capabilities ||
      this.receiving ||
      this.receiver
    )
      return;
    // Establish both media sections together. esp_peer supports one audio
    // and one video track; it cannot add another section after opening.
    // An audio-only peer (including our own microphone) can be announced first.
    // Choose a complete avatar pair instead of waiting for that peer's video.
    const video = tracks.find(
      (track) =>
        track.mediaTag === "cam-video" &&
        tracks.some(
          (candidate) =>
            candidate.mediaTag === "cam-audio" &&
            candidate.peerId === track.peerId,
        ),
    );
    const audio = tracks.find(
      (track) =>
        track.mediaTag === "cam-audio" && track.peerId === video?.peerId,
    );
    if (!audio || !video) return;
    console.info(
      "[Daily] Subscribing to avatar",
      this.options.receiveVideo === false ? "audio" : "audio/video",
    );
    this.receiving = this.subscribe(
      audio,
      this.options.receiveVideo === false ? null : video,
    )
      .catch((error: unknown) => {
        if (!this.closed)
          this.options.onError(
            error instanceof Error
              ? error
              : new Error("Daily media negotiation failed."),
          );
      })
      .finally(() => {
        this.receiving = null;
      });
  }

  private assertOpen(): void {
    if (this.closed) throw new Error("Daily media was closed.");
  }

  private watch(peer: RTCPeerConnection): void {
    peer.onconnectionstatechange = () => {
      if (!this.closed && peer.connectionState === "failed") {
        this.options.onError(new Error("Daily media connection failed."));
      }
    };
  }

  private async subscribe(
    audio: DailyTrack,
    video: DailyTrack | null,
  ): Promise<void> {
    const transport = this.transport;
    const capabilities = this.capabilities;
    if (!transport || !capabilities)
      throw new Error("Daily receive transport is not ready.");
    const consumers: Consumer[] = [];
    for (const track of video ? [audio, video] : [audio]) {
      const result = await this.options.signaling.request("recv-track", {
        mediaTag: track.mediaTag,
        mediaPeerId: track.peerId,
        rtpCapabilities: capabilities,
      });
      this.assertOpen();
      consumers.push(parseConsumer(result.consumerParameters));
    }
    this.consumers = consumers;
    const peer = this.options.createPeer();
    this.receiver = peer;
    this.watch(peer);
    peer.ontrack = (event) => {
      if (!this.closed) this.options.onTrack(audio.peerId, event.track);
    };
    peer.addTransceiver("audio", { direction: "recvonly" });
    if (video) peer.addTransceiver("video", { direction: "recvonly" });
    await peer.setRemoteDescription({
      type: "offer",
      sdp: receiveOffer(transport, consumers, 1),
    });
    this.assertOpen();
    const answer = await peer.createAnswer();
    this.assertOpen();
    if (!answer.sdp)
      throw new Error("The local peer returned an empty answer.");
    await peer.setLocalDescription(answer);
    this.assertOpen();
    await this.options.signaling.request("connect-transport", {
      transportId: transport.id,
      dtlsParameters: localDtlsParameters(answer.sdp),
    });
    this.assertOpen();
    // Both consumers are negotiated on the same connected transport. Send
    // both resumes before waiting: starting audio can occupy an embedded
    // device long enough to delay its acknowledgement and strand video.
    await Promise.all(
      consumers.map((consumer) =>
        this.options.signaling.request("resume-consumer", {
          consumerId: consumer.id,
        }),
      ),
    );
    this.assertOpen();
  }

  private async publish(track: MediaStreamTrack): Promise<void> {
    const result = await this.options.signaling.request("create-transport", {
      direction: "send",
    });
    this.assertOpen();
    const transport = parseTransport(result.transportOptions);
    const peer = this.options.createPeer();
    this.sender = peer;
    this.microphone = track;
    this.watch(peer);
    peer.addTransceiver(track, { direction: "sendonly" });
    const offer = await peer.createOffer();
    this.assertOpen();
    if (!offer.sdp)
      throw new Error("The local peer returned an empty microphone offer.");
    const rtpParameters = microphoneParameters(offer.sdp);
    await peer.setLocalDescription(offer);
    this.assertOpen();
    await this.options.signaling.request("connect-transport", {
      transportId: transport.id,
      dtlsParameters: localDtlsParameters(offer.sdp),
    });
    this.assertOpen();
    const produced = await this.options.signaling.request("send-track", {
      transportId: transport.id,
      kind: "audio",
      rtpParameters,
      paused: !track.enabled,
      appData: { mediaTag: "cam-audio" },
    });
    this.assertOpen();
    const producer = produced.producerInfo;
    if (
      typeof producer !== "object" ||
      producer === null ||
      !("id" in producer) ||
      typeof producer.id !== "string"
    ) {
      throw new Error("The SFU did not create the microphone producer.");
    }
    this.producerId = producer.id;
    console.info("[Daily] Applying microphone answer");
    await peer.setRemoteDescription({
      type: "answer",
      sdp: microphoneAnswer(transport, rtpParameters),
    });
    console.info("[Daily] Microphone answer applied");
    this.assertOpen();
    this.options.signaling.setMicrophoneEnabled(track.enabled);
    console.info("[Daily] Microphone publication completed");
  }

  setMicrophoneEnabled(enabled: boolean): void {
    if (!this.microphone || this.closed) return;
    // Stop capture at the track immediately, including while a server RPC
    // is pending. Never wait for signaling before honoring mute/Stop.
    this.microphone.enabled = enabled;
    this.options.signaling.setMicrophoneEnabled(enabled);
    if (this.producerId) {
      void this.options.signaling
        .request(enabled ? "resume-producer" : "pause-producer", {
          producerId: this.producerId,
        })
        .catch((error: unknown) => {
          if (!this.closed)
            this.options.onError(
              error instanceof Error
                ? error
                : new Error("Could not update microphone state."),
            );
        });
    }
  }

  consumerClosed(id: string): void {
    if (!this.closed && this.consumers.some((consumer) => consumer.id === id)) {
      this.options.onError(
        new Error("The avatar media track ended. Reconnect to continue."),
      );
    }
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    if (this.microphone) this.microphone.enabled = false;
    for (const peer of [this.sender, this.receiver]) {
      if (!peer) continue;
      peer.ontrack = null;
      peer.onconnectionstatechange = null;
      peer.close();
    }
    this.sender = null;
    this.receiver = null;
    this.microphone = null;
    this.consumers = [];
  }
}
