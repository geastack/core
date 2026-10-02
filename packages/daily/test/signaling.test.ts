import assert from "node:assert/strict";
import test from "node:test";
import {
  DailySignaling,
  parseRoomAddress,
  parseRoomLookup,
  roomLookupRequest,
} from "../src/index.ts";
import type {
  DailyClock,
  DailySocket,
  DailySignalingOptions,
  DailyTrack,
  DailyPresence,
} from "../src/index.ts";

class Clock implements DailyClock {
  private time = 0;
  private serial = 0;
  readonly timers = new Map<number, { at: number; callback: () => void }>();

  now(): number {
    return this.time;
  }

  later(callback: () => void, milliseconds: number): number {
    const id = ++this.serial;

    this.timers.set(id, { at: this.time + milliseconds, callback });

    return id;
  }

  cancel(timer: number): void {
    this.timers.delete(timer);
  }

  advance(milliseconds: number): void {
    const end = this.time + milliseconds;

    for (;;) {
      const next = [...this.timers]
        .filter(([, timer]) => timer.at <= end)
        .sort((left, right) => left[1].at - right[1].at)[0];

      if (!next) {
        break;
      }

      this.time = next[1].at;
      this.timers.delete(next[0]);
      next[1].callback();
    }

    this.time = end;
  }
}

interface SentMessage {
  msgStr: string;
  msgData: Record<string, unknown>;
  mtgStr?: string;
  from?: string;
}

class Socket implements DailySocket {
  onopen: (() => void) | null = null;
  onmessage: ((event: { data: string }) => void) | null = null;
  onerror: (() => void) | null = null;
  onclose: (() => void) | null = null;
  readonly sent: SentMessage[] = [];
  closed = false;
  respond: ((message: SentMessage) => void) | null = null;

  send(data: string): void {
    const message = JSON.parse(data) as SentMessage;

    this.sent.push(message);
    this.respond?.(message);
  }

  receive(value: object): void {
    this.onmessage?.({ data: JSON.stringify(value) });
  }

  close(): void {
    this.closed = true;
    this.onclose?.();
  }
}

const address = { domain: "test", room: "room" };
const room = parseRoomLookup(address, {
  worker: { workerId: "worker", wssUri: "wss://worker.daily.co" },
  sigAuthz: "credential",
});

function setup(callbacks: Partial<DailySignalingOptions> = {}) {
  const socket = new Socket();
  const clock = new Clock();
  const errors: Error[] = [];
  const signaling = new DailySignaling({
    socket: () => socket,
    clock,
    sessionId: "local",
    meetingSessionId: "meeting",
    userName: "Gea",
    client: { library: "gea", version: "0.0.1" },
    onError: (error) => errors.push(error),
    ...callbacks,
  });

  return { signaling, socket, clock, errors };
}

async function join(state: ReturnType<typeof setup>): Promise<void> {
  const connected = state.signaling.connect(room);

  state.socket.onopen?.();
  state.socket.receive({ msgStr: "sig-ack", sigAuthz: "refreshed" });
  await connected;
}

test("room lookup keeps credentials in the POST body and rejects other hosts", () => {
  assert.deepEqual(parseRoomAddress("https://test.daily.co/room/"), address);
  for (const url of [
    "http://test.daily.co/room",
    "https://test.daily.co.evil/room",
    "https://test.daily.co/a/b",
    "https://test.daily.co/../secret",
    "https://user@test.daily.co/room",
  ]) {
    assert.throws(() => parseRoomAddress(url));
  }

  const request = roomLookupRequest(address, "a+b&c", "session", {
    library: "gea",
    version: "1",
  });

  assert.equal(request.url, "https://gs.daily.co/rooms/check/test/room");
  assert.ok(request.body.includes("joinToken=a%2Bb%26c"));
  assert.throws(
    () => parseRoomLookup(address, { error: "secret token" }),
    /authorize/,
  );
  assert.throws(
    () => parseRoomLookup(address, { ...room, needToRequest: true }),
    /authorize/,
  );
});

test("join waits for acknowledgement and installs RPC correlation before send", async () => {
  const state = setup();

  await assert.rejects(
    state.signaling.request("create-transport", {}),
    /not joined/,
  );
  await join(state);
  assert.equal(state.socket.sent[0]?.mtgStr, "test/room");
  assert.equal(state.signaling.authorization, "refreshed");
  state.socket.respond = (message) => {
    state.socket.receive({
      tag: "soup",
      msgData: {
        _kTs: message.msgData._sendTs,
        transportOptions: { id: "recv" },
      },
    });
  };
  assert.deepEqual(
    await state.signaling.request("create-transport", { direction: "recv" }),
    {
      _kTs: "0.1",
      transportOptions: { id: "recv" },
    },
  );
  state.signaling.close();
  assert.equal(state.clock.timers.size, 0);
});

test("out-of-order responses stay with their own request; expired replies are ignored", async () => {
  const state = setup({ requestTimeoutMs: 100 });

  await join(state);
  const first = state.signaling.request("create-transport", {
    direction: "recv",
  });
  const second = state.signaling.request("create-transport", {
    direction: "send",
  });

  state.socket.receive({ tag: "soup", msgData: { _kTs: "0.2", id: "send" } });
  assert.equal((await second).id, "send");
  const rejected = assert.rejects(first, /timed out/);

  state.clock.advance(100);
  await rejected;
  state.socket.receive({ tag: "soup", msgData: { _kTs: "0.1", id: "late" } });
  state.signaling.close();
  assert.equal(state.errors.length, 0);
  assert.equal(state.clock.timers.size, 0);
});

for (const [response, reason] of [
  [{ error: "no compatible media codecs" }, "no compatible media codecs"],
  [
    { error: { message: "no compatible media codecs", code: 400 } },
    "no compatible media codecs",
  ],
  [{ error: { code: 400 } }, "Unknown server error."],
  [{ canceled: true }, "Request canceled."],
] as const) {
  test(`consumer rejection identifies the operation and server reason: ${JSON.stringify(response)}`, async () => {
    const state = setup();

    await join(state);
    const pending = state.signaling.request("recv-track", {
      mediaTag: "cam-video",
    });
    const rejected = assert.rejects(pending, {
      message: `Daily recv-track rejected: ${reason}`,
    });

    state.socket.receive({
      tag: "soup",
      msgData: { _kTs: "0.1", ...response },
    });
    await rejected;
    state.signaling.close();
    assert.equal(state.clock.timers.size, 0);
  });
}

test("Stop during join rejects immediately and detaches stale callbacks", async () => {
  const state = setup();
  const connected = state.signaling.connect(room);
  const rejected = assert.rejects(connected, /closed/);
  const stale = state.socket.onmessage;

  state.signaling.close();
  await rejected;
  stale?.({ data: '{"msgStr":"sig-ack"}' });
  assert.equal(state.clock.timers.size, 0);
  assert.equal(state.socket.closed, true);
  await assert.rejects(
    state.signaling.request("join-as-new-peer", {}),
    /not joined/,
  );
});

test("server close rejects all pending RPCs and clears timers", async () => {
  const state = setup();

  await join(state);
  const one = assert.rejects(
    state.signaling.request("create-transport", {}),
    /closed/,
  );
  const two = assert.rejects(
    state.signaling.request("recv-track", {}),
    /closed/,
  );

  state.socket.onclose?.();
  await Promise.all([one, two]);
  assert.equal(state.clock.timers.size, 0);
  assert.equal(state.errors.length, 1);
});

test("batched presence, app messages and audio/video announcements use the wire shapes", async () => {
  const tracks: (readonly DailyTrack[])[] = [];
  const presences: DailyPresence[] = [];
  const appMessages: unknown[] = [];
  const state = setup({
    onTracks: (value) => tracks.push(value),
    onPresence: (value) => presences.push(value),
    onAppMessage: (from, message) => appMessages.push({ from, message }),
  });

  await join(state);
  state.socket.receive({
    msgStr: "sig-batch",
    msgs: [
      { msgStr: "sig-presence", from: "bot", msgData: { name: "Zuck" } },
      { tag: "x-egassem", from: "bot", msgData: { type: "bot_ready" } },
      {
        tag: "soup",
        msgData: {
          tracks: [
            ["bot", "cam-audio"],
            ["bot", "cam-video"],
          ],
        },
      },
    ],
  });
  assert.deepEqual(tracks[0], [
    { peerId: "bot", mediaTag: "cam-audio" },
    { peerId: "bot", mediaTag: "cam-video" },
  ]);
  assert.deepEqual(appMessages, [
    { from: "bot", message: { type: "bot_ready" } },
  ]);
  state.socket.receive({ tag: "soup", msgData: { ptracks: { bot: [] } } });
  state.socket.receive({ msgStr: "sig-presence", from: "bot", msgData: null });
  assert.deepEqual(tracks[1], []);
  assert.equal(presences[1]?.disconnected, true);
  state.signaling.sendAppMessage({ event: "force-end" });
  assert.deepEqual(state.socket.sent.at(-1), {
    msgStr: "sig-msg",
    msgData: {
      tag: "x-egassem",
      fields: { event: "force-end" },
      envelopeTo: "*",
    },
  });
  state.signaling.close();
});

test("silent connection times out instead of looking connected forever", async () => {
  const state = setup();

  await join(state);
  state.clock.advance(2000);
  assert.equal(state.socket.sent.at(-1)?.msgStr, "sig-ack");
  state.socket.receive({ msgStr: "sig-ack" });
  state.clock.advance(10000);
  assert.equal(state.socket.closed, false);
  state.clock.advance(2000);
  assert.equal(state.socket.closed, true);
  assert.match(state.errors[0]?.message ?? "", /heartbeat/);
  assert.equal(state.clock.timers.size, 0);
});

test("malformed wire data and worker migration close cleanly", async () => {
  for (const message of [
    "not-json",
    JSON.stringify({ moveMeeting: { wssUri: "wss://other.daily.co" } }),
  ]) {
    const state = setup();

    await join(state);
    state.socket.onmessage?.({ data: message });
    assert.equal(state.socket.closed, true);
    assert.equal(state.errors.length, 1);
    assert.equal(state.clock.timers.size, 0);
  }
});
