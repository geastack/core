/** Web worker APIs implemented by the native host. */
export type WorkerMessage = string | ArrayBuffer

export interface MessageEvent<T = WorkerMessage> {
  readonly data: T
  readonly ports: MessagePort[]
}

export interface WorkerErrorEvent {
  readonly message: string
}

export interface MessagePort {
  onmessage: ((event: MessageEvent) => void) | null
  /** @gea-host-no-property-writes */
  postMessage(message: WorkerMessage, transfer?: (ArrayBuffer | MessagePort)[]): void
  start(): void
  close(): void
}

export interface MessageChannel {
  readonly port1: MessagePort
  readonly port2: MessagePort
}

export interface MessageChannelConstructor {
  new (): MessageChannel
}

export interface WorkerOptions {
  type?: 'classic' | 'module'
  name?: string
  credentials?: 'omit' | 'same-origin' | 'include'
}

export interface Worker {
  onmessage: ((event: MessageEvent) => void) | null
  onerror: ((event: WorkerErrorEvent) => void) | null
  postMessage(message: WorkerMessage, transfer?: (ArrayBuffer | MessagePort)[]): void
  terminate(): void
}

export interface WorkerConstructor {
  new (scriptURL: string | URL, options?: WorkerOptions): Worker
}

export interface DedicatedWorkerGlobalScope {
  onmessage: ((event: MessageEvent) => void) | null
  postMessage(message: WorkerMessage, transfer?: (ArrayBuffer | MessagePort)[]): void
  close(): void
}

declare global {
  interface ImportMeta {
    readonly url: string
  }

  interface URL {
    readonly href: string
    toString(): string
  }

  const URL: { new (url: string | URL, base?: string | URL): URL }
  const Worker: WorkerConstructor
  const MessageChannel: MessageChannelConstructor
  const self: DedicatedWorkerGlobalScope
}
