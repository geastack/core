import { AudioWorkletProcessor, registerProcessor } from '../runtime/audio-worklet'

// TypeScript's abstract modifier does not make the JavaScript constructor
// disappear. Its missing process implementation needs the ordinary native
// callable-family refusal, rather than an uninstantiable-class shortcut.
registerProcessor('abstract-through-any', AudioWorkletProcessor as any)
