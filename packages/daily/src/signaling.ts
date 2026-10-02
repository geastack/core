import { isRecord } from "./types.ts";
import type {
  DailyCommand,
  DailyRoom,
  DailySignalingOptions,
  DailySocket,
  DailyTrack,
} from "./types.ts";

interface PendingRequest {
  command?: DailyCommand;
  timer: number;
  resolve(value: Record<string, unknown>): void;
  reject(error: Error): void;
}

/** Daily 0.92.2's signaling and Mediasoup RPC share one WebSocket. */
export class DailySignaling {
  private socket: DailySocket | null = null;
  private readonly pending = new Map<string, PendingRequest>();
  private readonly tracks = new Map<string, string[]>();
  private joined = false;
  private joinPending: PendingRequest | null = null;
  private heartbeat: number | null = null;
  private lastReceived = 0;
  private sequence = 0;
  private authz = "";

  constructor(private readonly options: DailySignalingOptions) {}

  get authorization(): string {
    return this.authz;
  }

  async connect(room: DailyRoom): Promise<void> {
    if (this.socket) {
      throw new Error("Daily signaling is already connecting or connected.");
    }

    const socket = this.options.socket(room.worker.wssUri);

    this.socket = socket;
    this.authz = room.sigAuthz;
    this.lastReceived = this.options.clock.now();
    const joined = new Promise<Record<string, unknown>>((resolve, reject) => {
      this.joinPending = {
        resolve,
        reject,
        timer: this.options.clock.later(
          () => this.fail(new Error("Daily signaling join timed out.")),
          this.options.requestTimeoutMs ?? 15000,
        ),
      };
    });

    socket.onopen = () => {
      if (this.socket !== socket) {
        return;
      }

      try {
        console.info("[Daily] Sending signaling join");
        socket.send(
          JSON.stringify({
            mtgStr: `${room.address.domain}/${room.address.room}`,
            from: this.options.sessionId,
            msgStr: "join-for-sig",
            msgData: {
              sfuRequired: true,
              sigAuthz: this.authz,
              presence: {
                id: this.options.sessionId,
                name: this.options.userName,
                audioState: "U",
                videoState: "U",
                videoSourceDisabled: true,
              },
              mtgSession: { id: this.options.meetingSessionId },
              aboutClient: this.options.client,
            },
          }),
        );
        this.scheduleHeartbeat();
      } catch {
        this.fail(new Error("Could not send Daily signaling join."));
      }
    };
    socket.onmessage = (event) => {
      if (this.socket !== socket) {
        return;
      }

      try {
        console.info("[Daily] Incoming signal bytes", event.data.length);
        const message: unknown = JSON.parse(event.data);

        this.lastReceived = this.options.clock.now();
        this.receive(message, 0);
      } catch {
        this.fail(new Error("Invalid Daily signaling message."));
      }
    };
    socket.onerror = () => {
      if (this.socket === socket) {
        this.fail(new Error("Daily signaling connection failed."));
      }
    };
    socket.onclose = () => {
      if (this.socket === socket) {
        this.fail(new Error("Daily signaling connection closed."));
      }
    };

    await joined;
  }

  request(
    command: DailyCommand,
    fields: object,
  ): Promise<Record<string, unknown>> {
    if (!this.joined || !this.socket) {
      return Promise.reject(new Error("Daily signaling is not joined."));
    }

    // Register before send: an in-process transport can reply synchronously.
    const key = `${this.options.clock.now()}.${++this.sequence}`;

    return new Promise((resolve, reject) => {
      const timer = this.options.clock.later(() => {
        this.pending.delete(key);
        reject(new Error(`Daily ${command} timed out.`));
      }, this.options.requestTimeoutMs ?? 15000);

      this.pending.set(key, { command, timer, resolve, reject });
      try {
        this.send(command, { ...fields, _sendTs: key });
      } catch {
        this.options.clock.cancel(timer);
        this.pending.delete(key);
        reject(new Error(`Could not send Daily ${command}.`));
      }
    });
  }

  sendAppMessage(message: object, to = "*"): void {
    this.send("sig-msg", { tag: "x-egassem", fields: message, envelopeTo: to });
  }

  setMicrophoneEnabled(enabled: boolean): void {
    this.send("sig-presence", { audioState: enabled ? "" : "U" });
  }

  close(): void {
    this.dispose(new Error("Daily signaling was closed."));
  }

  private send(command: string, fields: object): void {
    if (!this.socket || !this.joined) {
      throw new Error("Daily signaling is not joined.");
    }

    this.socket.send(JSON.stringify({ msgStr: command, msgData: fields }));
  }

  private receive(value: unknown, depth: number): void {
    if (!isRecord(value) || depth > 8) {
      throw new Error("Malformed signaling envelope.");
    }

    console.info(
      "[Daily] Signal",
      typeof value.msgStr === "string" ? value.msgStr : "-",
      typeof value.tag === "string" ? value.tag : "-",
    );

    if (value.msgStr === "sig-batch") {
      if (!Array.isArray(value.msgs) || value.msgs.length > 256) {
        throw new Error("Malformed signaling batch.");
      }

      for (const message of value.msgs) {
        this.receive(message, depth + 1);
      }

      return;
    }

    if (value.msgStr === "sig-ack") {
      if (typeof value.sigAuthz === "string") {
        this.authz = value.sigAuthz;
      }

      this.joined = true;
      const pending = this.joinPending;

      this.joinPending = null;
      if (pending) {
        this.options.clock.cancel(pending.timer);
        pending.resolve(value);
      }
    }

    if (
      value.msgStr === "room-deleted" ||
      value.moveMeeting ||
      (isRecord(value.msgData) && value.msgData.moveMeeting)
    ) {
      this.fail(
        new Error("The Daily room ended or moved. Reconnect to continue."),
      );

      return;
    }

    if (Array.isArray(value.presences)) {
      for (const presence of value.presences) {
        this.receivePresence(presence);
      }
    } else if (
      value.msgStr === "sig-presence" &&
      typeof value.from === "string"
    ) {
      if (value.msgData === null) {
        this.options.onPresence?.({
          id: value.from,
          name: "",
          disconnected: true,
        });
      } else if (isRecord(value.msgData)) {
        this.receivePresence({ ...value.msgData, id: value.from });
      }
    }

    if (value.tag === "x-egassem" || value.msgStr === "x-egassem") {
      this.options.onAppMessage?.(
        typeof value.from === "string" ? value.from : "",
        value.msgData,
      );
    }

    if (value.tag === "soup" && isRecord(value.msgData)) {
      this.receiveSoup(value.msgData);
    }
  }

  private receivePresence(value: unknown): void {
    if (!isRecord(value) || typeof value.id !== "string") {
      return;
    }

    this.options.onPresence?.({
      id: value.id,
      name: typeof value.name === "string" ? value.name : "",
      disconnected: false,
    });
  }

  private receiveSoup(data: Record<string, unknown>): void {
    const key = data._kTs;
    const pending = typeof key === "string" ? this.pending.get(key) : undefined;

    if (pending && typeof key === "string") {
      this.pending.delete(key);
      this.options.clock.cancel(pending.timer);
      if (data.error || data.canceled) {
        const reason =
          typeof data.error === "string"
            ? data.error
            : isRecord(data.error) && typeof data.error.message === "string"
              ? data.error.message
              : data.canceled
                ? "Request canceled."
                : "Unknown server error.";
        pending.reject(
          new Error(
            `Daily ${pending.command ?? "media request"} rejected: ${reason}`,
          ),
        );
      } else {
        pending.resolve(data);
      }
    }

    let changed = false;

    if (Array.isArray(data.tracks)) {
      this.tracks.clear();
      for (const entry of data.tracks) {
        if (
          Array.isArray(entry) &&
          typeof entry[0] === "string" &&
          typeof entry[1] === "string"
        ) {
          const tags = this.tracks.get(entry[0]) ?? [];

          if (!tags.includes(entry[1])) {
            tags.push(entry[1]);
          }

          this.tracks.set(entry[0], tags);
        }
      }

      changed = true;
    }

    if (isRecord(data.ptracks)) {
      for (const [peerId, tags] of Object.entries(data.ptracks)) {
        if (Array.isArray(tags)) {
          this.tracks.set(
            peerId,
            tags.filter((tag): tag is string => typeof tag === "string"),
          );
        } else if (tags === null) {
          this.tracks.delete(peerId);
        }
      }

      changed = true;
    }

    if (changed) {
      const tracks: DailyTrack[] = [];

      for (const [peerId, tags] of this.tracks) {
        for (const mediaTag of tags) {
          tracks.push({ peerId, mediaTag });
        }
      }

      this.options.onTracks?.(tracks);
    }

    if (typeof data.consumerClosed === "string") {
      this.options.onConsumerClosed?.(data.consumerClosed);
    }

    if (Array.isArray(data.consumersClosed)) {
      for (const id of data.consumersClosed) {
        if (typeof id === "string") {
          this.options.onConsumerClosed?.(id);
        }
      }
    }
  }

  private scheduleHeartbeat(): void {
    this.heartbeat = this.options.clock.later(() => {
      this.heartbeat = null;
      if (!this.socket) {
        return;
      }

      const silence = this.options.clock.now() - this.lastReceived;

      if (silence > 10000) {
        this.fail(new Error("Daily signaling heartbeat timed out."));

        return;
      }

      if (this.joined && silence >= 1900) {
        try {
          this.send("sig-ack", { lTs: this.options.clock.now() });
        } catch {
          this.fail(new Error("Could not send Daily heartbeat."));

          return;
        }
      }

      this.scheduleHeartbeat();
    }, 2000);
  }

  private fail(error: Error): void {
    this.dispose(error);
    this.options.onError?.(error);
  }

  private dispose(error: Error): void {
    const socket = this.socket;

    this.socket = null;
    this.joined = false;
    this.authz = "";
    this.tracks.clear();
    if (this.heartbeat !== null) {
      this.options.clock.cancel(this.heartbeat);
      this.heartbeat = null;
    }

    if (this.joinPending) {
      this.options.clock.cancel(this.joinPending.timer);
      this.joinPending.reject(error);
      this.joinPending = null;
    }

    for (const pending of this.pending.values()) {
      this.options.clock.cancel(pending.timer);
      pending.reject(error);
    }

    this.pending.clear();
    if (socket) {
      socket.onopen = null;
      socket.onmessage = null;
      socket.onerror = null;
      socket.onclose = null;
      socket.close();
    }
  }
}
