import type { MessagePort } from '../workers.js'

/** Native implementations of the AudioWorkletGlobalScope's two class APIs.
 * The module loader supplies these lexical bindings to compiled worklet modules;
 * applications use the browser globals, without imports or platform branches.
 */

declare function __geaAudioWorkletProcessorPort(): MessagePort
declare function __geaFlushAudioWorkletOutput(): void

/** Gea hardware cancellation extension; capture remains active. */
export function flushAudioWorkletOutput(): void {
  __geaFlushAudioWorkletOutput()
}

declare function __geaRegisterAudioWorkletProcessor<T extends AudioWorkletProcessor>(
  name: string,
  create: () => T,
  process: (processor: T, inputs: Float32Array[][], outputs: Float32Array[][]) => boolean,
): void

export abstract class AudioWorkletProcessor {
  readonly port: MessagePort

  constructor() {
    this.port = __geaAudioWorkletProcessorPort()
  }

  abstract process(
    inputs: Float32Array[][],
    outputs: Float32Array[][],
    parameters: Record<string, Float32Array>,
  ): boolean
}

// The native module builder validates the concrete registration constructor.
// AudioParam descriptors are explicitly unsupported, before code generation.
export function registerProcessor<T extends AudioWorkletProcessor>(
  name: string,
  constructor: new () => T,
): void {
  __geaRegisterAudioWorkletProcessor(
    name,
    () => new constructor(),
    (processor, inputs, outputs) => processor.process(inputs, outputs, {}),
  )
}
