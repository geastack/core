import { AudioWorkletProcessor, registerProcessor } from '../runtime/audio-worklet'

class ParameterProcessor extends AudioWorkletProcessor {
  static get parameterDescriptors(): readonly { name: string }[] {
    return [{ name: 'gain' }]
  }

  process(): boolean {
    return true
  }
}

registerProcessor('unsupported-parameters', ParameterProcessor)
