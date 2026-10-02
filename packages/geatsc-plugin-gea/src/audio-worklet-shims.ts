import { geaHostDeclarations } from './host-declarations.js'
import type { HostShimDefinitions } from './types.js'

export function extendAudioWorkletShims(shims: HostShimDefinitions): HostShimDefinitions {
  const declarations = [...geaHostDeclarations, '#include "gea/audio-worklet-runtime.h"']
  for (const [name, native, result] of [
    ['__geaAudioWorkletProcessorPort', 'gea::host::audio_worklet::processorPort', 'gea::host::workers::MessagePort'],
    ['__geaFlushAudioWorkletOutput', 'gea::host::audio_worklet::flushOutput', 'void'],
    ['__geaRegisterAudioWorkletProcessor', 'gea::runtime::hostaudio::registerTypedProcessor', 'void'],
  ]) {
    shims.embeddedHostFunctions![name] = native
    shims.embeddedHostFunctionReturnTypes![name] = result
    shims.hostExternDeclarations![native] = declarations
  }
  for (const type of ['AudioContext', 'AudioWorklet', 'AudioWorkletNode', 'MediaStreamAudioSourceNode']) {
    shims.nativeTypes![type] = `gea::host::${type}`
    shims.hostExternDeclarations![`gea::host::${type}`] = declarations
  }
  const receiverTypes = (type: string) => [type, `gea::host::${type}`]
  const method = (type: string, name: string, returnType: string, emit = `({receiver}).${name}({args})`) => {
    ;(shims.nativeMemberMethods![name] ||= []).push({ receiverTypes: receiverTypes(type), returnType, emit })
    shims.hostExternDeclarations![emit] = declarations
  }
  const property = (type: string, name: string, returnType: string, emit = `({receiver}).${name}`) => {
    ;(shims.nativeMemberPropertyGetters![name] ||= []).push({ receiverTypes: receiverTypes(type), returnType, emit })
    shims.hostExternDeclarations![emit] = declarations
  }
  for (const type of ['AudioContext', 'AudioWorkletNode']) {
    const construct = `gea::runtime::hostaudio::create${type}({args})`
    shims.nativeTypes![`${type}Constructor`] = `gea::host::${type}`
    shims.nativeConstructors![`${type}Constructor`] = construct
    shims.embeddedHostClasses![type] = { wrapper: `gea::host::${type}`, construct }
    shims.hostExternDeclarations![`gea::runtime::hostaudio::create${type}`] = declarations
  }
  property('AudioContext', 'sampleRate', 'double')
  property('AudioContext', 'baseLatency', 'double', '({receiver}).baseLatency()')
  property('AudioContext', 'outputLatency', 'double', '({receiver}).outputLatency()')
  property('AudioContext', 'state', 'std::string', '({receiver}).state()')
  property('AudioContext', 'audioWorklet', 'gea::host::AudioWorklet')
  property('AudioWorkletNode', 'port', 'gea::host::workers::MessagePort')
  method('AudioWorklet', 'addModule', 'gea::Promise<void>', 'gea::runtime::hostaudio::workletAddModule({receiver}, {arg0})')
  for (const name of ['resume', 'suspend', 'close'])
    method('AudioContext', name, 'gea::Promise<void>', `gea::runtime::hostaudio::context${name[0].toUpperCase() + name.slice(1)}({receiver})`)
  method('AudioContext', 'createMediaStreamSource', 'gea::host::MediaStreamAudioSourceNode')
  method('MediaStreamAudioSourceNode', 'connect', 'gea::host::AudioWorkletNode')
  method('AudioWorkletNode', 'connect', 'gea::host::AudioDestinationNode')
  for (const type of ['MediaStreamAudioSourceNode', 'AudioWorkletNode']) method(type, 'disconnect', 'void')
  ;(shims.nativeMemberPropertySetters!.onprocessorerror ||= []).push({
    receiverTypes: receiverTypes('AudioWorkletNode'), returnType: 'void',
    emit: 'gea::runtime::hostaudio::setOnProcessorError({receiver}, {value})',
  })
  for (const name of ['sampleRate', 'currentTime', 'currentFrame']) {
    shims.hostGlobalObjects![name] = `gea::host::audio_worklet::${name}()`
    shims.hostGlobalObjectTypes![name] = 'double'
    shims.embeddedHostConstants![name] = { emit: `gea::host::audio_worklet::${name}()`, type: 'double' }
    shims.hostExternDeclarations![`gea::host::audio_worklet::${name}()`] = declarations
  }
  return shims
}
