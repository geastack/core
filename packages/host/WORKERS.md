# Native workers

Gea compiles module workers into separate native entry points. A worker runs on
its own task and event loop, with its own compiler runtime realm. Its WebSocket
callbacks, timers, messages, and promise jobs do not depend on the UI frame loop.
The TypeScript interfaces use browser API names; there is no application-specific
transport or microphone pipe API.

## Supported API

- `new Worker(url, { type: 'module' })`, `postMessage`, `onmessage`, `onerror`,
  and `terminate`.
- Worker-global `self.onmessage`, `self.postMessage`, and `self.close`.
- `new MessageChannel()`, `port1`, `port2`, and each port's `onmessage`,
  `postMessage`, `start`, and `close`.
- String or `ArrayBuffer` message data, including a typed union of those types.
- Transfer lists containing `ArrayBuffer` and `MessagePort`; transferred ports
  arrive in `event.ports`.

This is a subset of the browser Worker and structured-clone APIs. Arbitrary
records, cyclic object graphs, classes, functions, typed-array messages, shared
memory, and other transferable types are not supported message payloads. Send
JSON strings for sparse structured control messages and an `ArrayBuffer` for
bulk binary data. Unsupported statically typed payloads fail compilation rather
than being boxed or shallow-copied across realms.

Use an explicit module option. The embedded runtime also accepts an omitted
option for compiled modules; an explicitly requested classic worker is rejected.
Worker URLs must identify modules included in the build. They do not dynamically
download or evaluate JavaScript. Literal URLs and `new URL('./worker.ts',
import.meta.url)` are discovered by the analyzer; dependencies within those
modules participate in native capability inference. The `worker-realms` feature
enables `GEA_RUNTIME_REALMS` automatically, including for AudioWorklet modules.

## Ownership and ordering

A normal `ArrayBuffer` message clones bytes. A transferred buffer moves its
native byte storage where possible and detaches the sender's buffer and its
views. Small inline buffers may require a copy when becoming owned queue storage.
The receiver creates its own realm-local `ArrayBuffer` wrapper. Compiler `Ref`
objects and their non-atomic reference counts never cross task boundaries.

Transferring a port invalidates all aliases to the sender's native ownership
handle. Its queued messages follow the port, but its old event handler does not.
The receiver adopts the endpoint on its own context. Generation checks prevent a
delivery already scheduled on the sender from invoking a callback after transfer.
Each endpoint preserves FIFO message order. Setting `onmessage` starts delivery;
`start()` is also available.

Closing a port drops its pending messages and handler. Callback captures are
released outside native locks, including when replacing handlers and transferring
ports. This permits capture destructors to close ports without reentrant
deadlocks. Context teardown clears all endpoints still owned by that context,
including native-port/compiled-object callback cycles. Endpoints transferred
onward belong to the new owner's cleanup.

## Scheduling and bounded queues

Both a context's posted-task queue and each port's pending-message queue are
bounded to **1,024 entries and 2 MiB of accounted payload bytes**. The byte bound
counts payload bytes, not allocator overhead or arbitrary callback captures.
Queue overflow raises an explicit error; it does not silently drop audio or
grow an unlimited backlog. Context posting returns failure when full or stopped.
An overflowed transfer send is not a retryable ownership transaction: transferred
buffers may already be detached when the native queue rejects it. Treat overflow
as a transport failure and use a fresh buffer when recovering.

Timers use a monotonic clock. Ready timers alternate with queued tasks so a
continuous message stream cannot indefinitely starve timers. Repeating timers
have a minimum one-millisecond interval. The ordinary worker loop executes work
in bounded scheduling slices, then waits for an event or the next timer deadline;
idle workers do not poll the UI. A callback itself runs to completion and is not
preempted by this scheduling budget.

Desktop contexts run on dedicated native threads. ESP contexts use dedicated
FreeRTOS tasks with a 16 KiB stack and default priority 8. The host audio runtime
requests the higher task priority needed for its render deadline. That priority
choice is native host policy, not an extra JavaScript API.

AudioWorklet owns a render loop and uses the same context primitives to pump
messages on its task. A direct `MessageChannel` between a network worker and an
audio processor can therefore keep both capture and playback off the UI loop.

## Shutdown and errors

`terminate()` immediately prevents new worker deliveries and requests shutdown.
Native task shutdown is **cooperative at callback boundaries**. It cannot abort
an infinite loop or a blocking native call already executing inside a callback;
this differs from a browser's forcibly terminated worker execution. Worker-global
`close()` requests the same event-loop exit after the current callback.

Pending callbacks and timers, native sockets, and owned port handlers are cleared
on their creating task before its compiler realm is destroyed. Uncaught callback
exceptions are forwarded to the creator's `onerror` handler through that creator's
queue. The creator's handlers and compiled references are only acquired or invoked
on the creator context.

Host integrations that supply their own task loop must bind `ContextScope`, run
pending events on that task, and call `Context::finish()` before destroying its
`RuntimeRealm`. Calling `stop()` alone requests shutdown; it does not perform
owner-thread cleanup.

## Targeted verification

`packages/core/test/test_worker_runtime.cpp` checks worker messages, WebSocket
callbacks, and timers while the UI sleeps for 300 ms, as well as transfers,
stale scheduled deliveries, queue bounds, static registration, shutdown, and
reentrant capture destruction.

`packages/core/test/test_worker_bridge.cpp` checks typed compiler events, buffer
cloning and detachment, and owner-thread cleanup of an actual compiled `Ref`
capture cycle. Run native tests with address and undefined-behavior sanitizers
when changing ownership or teardown code. Analyzer coverage is in
`packages/geatsc-plugin-gea/test/analyze-workers.test.mjs`.
