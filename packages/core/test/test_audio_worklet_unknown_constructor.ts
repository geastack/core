import { AudioWorkletProcessor, registerProcessor } from '../runtime/audio-worklet'

declare const unknownConstructor: new () => AudioWorkletProcessor

// The native host cannot establish which prototype body this constructor
// supplies merely from its base-class signature.
registerProcessor('unknown-constructor', unknownConstructor)
