import { geaHostDeclarations } from './host-declarations.js'
import type { HostShimDefinitions } from './types.js'

export function extendWorkerShims(shims: HostShimDefinitions): HostShimDefinitions {
  const namespace = 'gea::host::workers'
  const bridge = 'gea::runtime::hostworker'
  const declarations = [...geaHostDeclarations, '#include "gea/worker-runtime.h"']
  const types = ['Worker', 'MessageChannel', 'MessagePort', 'DedicatedWorkerGlobalScope']

  for (const type of types) {
    const native = `${namespace}::${type}`

    shims.nativeTypes![type] = native
    shims.hostExternDeclarations![native] = declarations
  }

  const receivers = (type: string) => [type, shims.nativeTypes![type]!]
  const method = (type: string, name: string, result: string, emit: string) => {
    ;(shims.nativeMemberMethods![name] ||= []).push({ receiverTypes: receivers(type), returnType: result, emit })
    shims.hostExternDeclarations![emit] = declarations
  }
  const setter = (type: string, name: string, emit: string) => {
    ;(shims.nativeMemberPropertySetters![name] ||= []).push({ receiverTypes: receivers(type), returnType: 'void', emit })
    shims.hostExternDeclarations![emit] = declarations
  }

  shims.nativeTypes!.WorkerConstructor = `${namespace}::Worker`
  shims.nativeTypes!.MessageChannelConstructor = `${namespace}::MessageChannel`
  shims.nativeConstructors!.WorkerConstructor = `${bridge}::createWorker({args})`
  shims.nativeConstructors!.MessageChannelConstructor = `${namespace}::MessageChannel()`
  shims.hostExternDeclarations![`${bridge}::createWorker`] = declarations
  shims.hostGlobalObjects!.self = `${namespace}::self()`
  shims.hostGlobalObjectTypes!.self = `${namespace}::DedicatedWorkerGlobalScope`
  shims.embeddedHostConstants!.self = {
    emit: `${namespace}::self()`,
    type: `${namespace}::DedicatedWorkerGlobalScope`,
  }
  shims.hostExternDeclarations![`${namespace}::self()`] = declarations

  for (const type of ['Worker', 'MessagePort', 'DedicatedWorkerGlobalScope']) {
    method(type, 'postMessage', 'void', `${bridge}::postMessage({receiver}, {args})`)
    setter(type, 'onmessage', `${bridge}::setOnMessage({receiver}, {value})`)
  }

  setter('Worker', 'onerror', `${bridge}::setOnError({receiver}, {value})`)
  method('Worker', 'terminate', 'void', '({receiver}).terminate()')
  method('MessagePort', 'start', 'void', '({receiver}).start()')
  method('MessagePort', 'close', 'void', '({receiver}).close()')
  method('DedicatedWorkerGlobalScope', 'close', 'void', `${namespace}::Context::current()->stop()`)

  for (const name of ['port1', 'port2']) {
    ;(shims.nativeMemberPropertyGetters![name] ||= []).push({
      receiverTypes: receivers('MessageChannel'), returnType: `${namespace}::MessagePort`, emit: `({receiver}).${name}`,
    })
  }

  return shims
}
