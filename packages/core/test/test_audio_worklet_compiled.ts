import {
  AudioWorkletProcessor,
  registerProcessor
} from '../runtime/audio-worklet'

class CopyProcessor extends AudioWorkletProcessor {
  process(inputs: Float32Array[][], outputs: Float32Array[][]): boolean {
    const input = inputs[0][0]
    const output = outputs[0][0]
    for (let index = 0; index < output.length; index += 1)
      output[index] = input[index]
    return true
  }
}

registerProcessor('typed-copy', CopyProcessor)
