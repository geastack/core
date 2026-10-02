import { DailyMedia } from "./media.ts";
import { DailySignaling } from "./signaling.ts";
import {
  parseRoomAddress,
  parseRoomLookup,
  roomLookupRequest,
} from "./room.ts";
import type {
  DailyClock,
  DailyPresence,
  DailySocket,
  DailyTrack,
} from "./types.ts";

export interface DailyClientOptions {
  receiveVideo?: boolean;
  socket(url: string): DailySocket;
  clock: DailyClock;
  uuid(): string;
  post(url: string, contentType: string, body: string): Promise<unknown>;
  createPeer(): RTCPeerConnection;
  microphone(): Promise<MediaStream>;
  onAppMessage(from: string, data: unknown): void;
  onPresence(presence: DailyPresence): void;
  onTrack(peerId: string, track: MediaStreamTrack): void;
  onError(error: Error): void;
}

/** Hosted Daily room client: direct HTTPS + WSS + two native WebRTC peers. */
export class DailyClient {
  private signaling: DailySignaling | null = null;
  private media: DailyMedia | null = null;
  private microphone: MediaStream | null = null;
  private closed = false;
  private audioEnabled = true;
  private tracks: readonly DailyTrack[] = [];

  constructor(private readonly options: DailyClientOptions) {}

  async join(
    url: string,
    token: string,
    name: string,
    audioEnabled = true,
  ): Promise<void> {
    if (this.closed || this.signaling)
      throw new Error("This Daily client has already been used.");
    this.audioEnabled = audioEnabled;
    const client = { library: "gea", version: "0.0.1" };
    const sessionId = this.options.uuid();
    const address = parseRoomAddress(url);
    const request = roomLookupRequest(address, token, sessionId, client);
    const discovery = await this.options.post(
      request.url,
      request.contentType,
      request.body,
    );
    if (this.closed) throw new Error("Daily join was cancelled.");
    const room = parseRoomLookup(address, discovery);
    const signaling = new DailySignaling({
      socket: this.options.socket,
      clock: this.options.clock,
      client,
      sessionId,
      meetingSessionId: this.options.uuid(),
      userName: name,
      onAppMessage: (from, data) => this.options.onAppMessage(from, data),
      onPresence: (presence) => this.options.onPresence(presence),
      onError: (error) => this.options.onError(error),
      onTracks: (tracks) => {
        this.tracks = tracks;
        this.media?.updateTracks(tracks);
      },
      onConsumerClosed: (id) => this.media?.consumerClosed(id),
    });
    this.signaling = signaling;
    try {
      await signaling.connect(room);
      const joined = await signaling.request("join-as-new-peer", {
        h264Profile: "42e01f",
        sigAuthz: signaling.authorization,
      });
      if (this.closed) throw new Error("Daily join was cancelled.");
      const media = new DailyMedia({
        receiveVideo: this.options.receiveVideo,
        signaling,
        createPeer: this.options.createPeer,
        onTrack: (peerId, track) => this.options.onTrack(peerId, track),
        onError: (error) => this.options.onError(error),
      });
      this.media = media;
      media.updateTracks(this.tracks);
      if (audioEnabled) {
        const microphone = await this.options.microphone();
        if (this.closed) {
          for (const track of microphone.getTracks()) track.stop();
          throw new Error("Daily join was cancelled.");
        }
        this.microphone = microphone;
        for (const track of microphone.getAudioTracks())
          track.enabled = this.audioEnabled;
      }
      await media.start(
        joined.routerRtpCapabilities,
        this.microphone?.getAudioTracks()[0] ?? null,
      );
      console.info("[Daily] Join completed");
    } catch (error) {
      // Retain signaling so the application can still send force-end after
      // a media failure. The owner closes it once hosted cleanup completes.
      this.media?.close();
      this.stopMicrophone();
      throw error;
    }
  }

  setMicrophoneEnabled(enabled: boolean): void {
    this.audioEnabled = enabled;
    for (const track of this.microphone?.getAudioTracks() ?? [])
      track.enabled = enabled;
    this.media?.setMicrophoneEnabled(enabled);
  }

  sendAppMessage(message: object, to = "*"): void {
    if (!this.signaling || this.closed)
      throw new Error("Daily signaling is not connected.");
    this.signaling.sendAppMessage(message, to);
  }

  private stopMicrophone(): void {
    for (const track of this.microphone?.getTracks() ?? []) track.stop();
    this.microphone = null;
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.media?.close();
    this.stopMicrophone();
    this.signaling?.close();
    this.media = null;
    this.signaling = null;
    this.tracks = [];
  }
}
