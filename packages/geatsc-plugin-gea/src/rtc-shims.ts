import type { HostShimDefinitions } from './types.js'

// One mapping for the standard RTC objects. The app does not supply a plugin.
export function extendRtcShims(shims: HostShimDefinitions): HostShimDefinitions {
  const types = ['RTCDataChannel', 'RTCRtpTransceiver', 'RTCRtpSender', 'RTCRtpReceiver', 'RTCIceCandidate', 'RTCTrackEvent']
  for (const type of types) shims.nativeTypes![type] = `gea::host::${type}`
  shims.nativeTypes!.RTCSessionDescription = 'gea::host::RTCSessionDescriptionObject'
  shims.nativeTypes!.RTCSessionDescriptionConstructor = 'gea::host::RTCSessionDescriptionObject'
  shims.nativeTypes!.RTCIceCandidateConstructor = 'gea::host::RTCIceCandidate'
  shims.nativeViews = {
    ...shims.nativeViews,
    'gea::host::HTMLVideoElement': {
      'gea::embedded::ui::NodeHandle': 'gea::host::HTMLVideoElement({value})',
    },
    'gea::host::HTMLAudioElement': {
      'gea::embedded::ui::NodeHandle': 'gea::host::HTMLAudioElement({value})',
    },
  }
  const receivers = (type: string) => [shims.nativeTypes![type] ?? `gea::host::${type}`, type, ...(type === 'RTCPeerConnection' ? ['RTCPeerConnectionInstance'] : [])]
  const get = (type: string, name: string, result: string, optional = false) => {
    const rows = (shims.nativeMemberPropertyGetters![name] ||= [])
    rows.push({ receiverTypes: receivers(type), returnType: result,
      emit: optional ? `gea::runtime::hostrtc::optional(({receiver}).${name}())` : `({receiver}).${name}()` })
  }
  const set = (type: string, name: string, method: string) => {
    const rows = (shims.nativeMemberPropertySetters![name] ||= [])
    rows.push({ receiverTypes: receivers(type), returnType: 'void', emit: `({receiver}).${method}({value})` })
  }
  const call = (type: string, name: string, result: string, emit = `({receiver}).${name}({args})`) => {
    const rows = (shims.nativeMemberMethods![name] ||= [])
    const prior = rows.findIndex(row => row.receiverTypes?.includes(type))
    const row = { receiverTypes: receivers(type), returnType: result, emit }
    if (prior >= 0) rows[prior] = row
    else rows.push(row)
  }
  shims.nativeTypes!.PcmAudioStream = 'gea::host::PcmAudioStream'
  shims.nativeTypes!.PcmAudioStreamConstructor = 'gea::host::PcmAudioStream'
  shims.nativeConstructors!.PcmAudioStreamConstructor = 'gea::host::PcmAudioStream::create({args})'
  for (const name of ['queuedMs', 'playedMs', 'audioLevel', 'capturePendingMs', 'captureDroppedSamples', 'capturePackets'])
    get('PcmAudioStream', name, 'double')
  get('PcmAudioStream', 'drained', 'bool')
  call('PcmAudioStream', 'readBase64', 'std::string')
  for (const name of ['setInput', 'pipeTo', 'receiveFrom', 'writeBase64', 'resetPlayback', 'close']) call('PcmAudioStream', name, 'void')
  get('WebSocketInstance', 'bufferedAmount', 'double')
  get('WebSocketInstance', 'readyState', 'double')
  get('RTCPeerConnection', 'signalingState', 'std::string')
  get('HTMLAudioElement', 'srcObject', 'gea::host::MediaStream')
  get('HTMLAudioElement', 'autoplay', 'bool')
  get('HTMLAudioElement', 'paused', 'bool')
  get('HTMLAudioElement', 'drained', 'bool')
  get('HTMLAudioElement', 'audioLevel', 'double')
  call('HTMLAudioElement', 'beginDrain', 'void')
  call('HTMLAudioElement', 'clearBufferedAudio', 'void')
  set('HTMLAudioElement', 'srcObject', 'setSrcObject')
  set('HTMLAudioElement', 'autoplay', 'setAutoplay')
  get('HTMLVideoElement', 'srcObject', 'gea::host::MediaStream')
  get('HTMLVideoElement', 'src', 'std::string')
  get('HTMLVideoElement', 'autoplay', 'bool')
  get('HTMLVideoElement', 'paused', 'bool')
  get('HTMLVideoElement', 'videoWidth', 'double')
  get('HTMLVideoElement', 'videoHeight', 'double')
  set('HTMLVideoElement', 'srcObject', 'setSrcObject')
  set('HTMLVideoElement', 'src', 'setSrc')
  set('HTMLVideoElement', 'autoplay', 'setAutoplay')
  set('HTMLVideoElement', 'onplaying', 'setOnPlaying')
  call('HTMLVideoElement', 'play', 'gea::Promise<void>', 'gea::runtime::hostaudio::play({receiver})')
  call('HTMLVideoElement', 'pause', 'void')
  get('RTCTrackEvent', 'track', 'gea::host::MediaStreamTrack')
  get('RTCTrackEvent', 'streams', 'std::vector<gea::host::MediaStream>')
  get('RTCTrackEvent', 'receiver', 'gea::host::RTCRtpReceiver')
  get('RTCTrackEvent', 'transceiver', 'gea::host::RTCRtpTransceiver')
  get('RTCSessionDescription', 'type', 'std::string')
  get('RTCSessionDescription', 'sdp', 'std::string')
  call('RTCSessionDescription', 'toJSON', 'gea::runtime::hostrtc::DescriptionSnapshot', 'gea::runtime::hostrtc::descriptionJson({receiver})')
  get('RTCIceCandidate', 'candidate', 'std::string')
  for (const name of ['sdpMid', 'foundation', 'component', 'address', 'protocol', 'type', 'tcpType', 'relatedAddress', 'usernameFragment', 'relayProtocol', 'url'])
    get('RTCIceCandidate', name, 'gea::Optional<std::string>', true)
  for (const name of ['sdpMLineIndex', 'priority', 'port', 'relatedPort']) get('RTCIceCandidate', name, 'gea::Optional<double>', true)
  call('RTCIceCandidate', 'toJSON', 'gea::runtime::hostrtc::CandidateSnapshot', 'gea::runtime::hostrtc::candidateJson({receiver})')
  get('MediaStream', 'active', 'bool')
  call('MediaStream', 'getVideoTracks', 'std::vector<gea::host::MediaStreamTrack>')
  call('MediaStream', 'getTrackById', 'gea::host::MediaStreamTrack')
  call('MediaStream', 'addTrack', 'void')
  call('MediaStream', 'removeTrack', 'void')
  for (const name of ['localDescription', 'remoteDescription']) {
    (shims.nativeMemberPropertyGetters![name] ||= []).push({
      receiverTypes: receivers('RTCPeerConnection'), returnType: 'gea::runtime::hostrtc::DescriptionSnapshot',
      emit: `gea::runtime::hostrtc::${name}({receiver})`,
    })
  }
  for (const [type, names] of [
    ['RTCDataChannel', ['label', 'protocol', 'readyState', 'binaryType']],
    ['RTCRtpTransceiver', ['direction']],
  ] as const) for (const name of names) get(type, name, 'std::string')
  for (const name of ['id', 'maxRetransmits', 'maxPacketLifeTime']) get('RTCDataChannel', name, 'gea::Optional<double>', true)
  for (const name of ['bufferedAmount', 'bufferedAmountLowThreshold']) get('RTCDataChannel', name, 'double')
  get('RTCDataChannel', 'ordered', 'bool')
  get('RTCRtpTransceiver', 'stopped', 'bool')
  for (const name of ['mid', 'currentDirection']) get('RTCRtpTransceiver', name, 'gea::Optional<std::string>', true)
  get('RTCRtpTransceiver', 'sender', 'gea::host::RTCRtpSender')
  get('RTCRtpTransceiver', 'receiver', 'gea::host::RTCRtpReceiver')
  // Nullable native handles use their zero handle, unlike nullable primitives
  // (which use Optional). State keeps std::optional; the JS ABI keeps the handle.
  for (const type of ['RTCRtpSender', 'RTCRtpReceiver']) {
    (shims.nativeMemberPropertyGetters!.track ||= []).push({
      receiverTypes: receivers(type), returnType: 'gea::host::MediaStreamTrack',
      emit: '({receiver}).track().value_or(gea::host::MediaStreamTrack{})',
    })
  }
  set('RTCDataChannel', 'binaryType', 'setBinaryType')
  set('RTCDataChannel', 'bufferedAmountLowThreshold', 'setBufferedAmountLowThreshold')
  set('RTCRtpTransceiver', 'direction', 'setDirection')
  call('RTCDataChannel', 'close', 'void')
  call('RTCRtpTransceiver', 'stop', 'void')
  call('RTCDataChannel', 'send', 'void', 'gea::runtime::hostrtc::send({receiver}, {arg0})')
  call('RTCRtpSender', 'replaceTrack', 'gea::Promise<void>', 'gea::runtime::hostrtc::replaceTrack({receiver}, {arg0})')
  call('RTCPeerConnection', 'addTrack', 'gea::host::RTCRtpSender')
  call('RTCPeerConnection', 'removeTrack', 'void')
  call('RTCPeerConnection', 'createDataChannel', 'gea::host::RTCDataChannel', 'gea::runtime::hostrtc::createDataChannel({receiver}, {args})')
  call('RTCPeerConnection', 'addTransceiver', 'gea::host::RTCRtpTransceiver', 'gea::runtime::hostrtc::addTransceiver({receiver}, {args})')
  for (const [method, type] of [['getSenders', 'RTCRtpSender'], ['getReceivers', 'RTCRtpReceiver'], ['getTransceivers', 'RTCRtpTransceiver']])
    call('RTCPeerConnection', method, `std::vector<gea::host::${type}>`)
  for (const [type, name, method] of [
    ['RTCDataChannel', 'onopen', 'setChannelOnOpen'],
    ['RTCDataChannel', 'onclose', 'setChannelOnClose'],
    ['RTCDataChannel', 'onbufferedamountlow', 'setChannelOnLow'],
    ['RTCDataChannel', 'onmessage', 'setChannelOnMessage'],
    ['RTCPeerConnection', 'ondatachannel', 'setPeerOnDataChannel'],
    ['RTCPeerConnection', 'ontrack', 'setPeerOnTrack'],
    ['RTCPeerConnection', 'onsignalingstatechange', 'setPeerOnSignaling'],
    ['RTCPeerConnection', 'onnegotiationneeded', 'setPeerOnNegotiation'],
  ]) {
    (shims.nativeMemberPropertySetters![name] ||= []).push({
      receiverTypes: receivers(type), returnType: 'void', emit: `gea::runtime::hostrtc::${method}({receiver}, {value})`,
    })
  }
  shims.nativeConstructors!.RTCPeerConnectionConstructor = 'gea::runtime::hostrtc::createPeer({args})'
  shims.embeddedHostClasses!.RTCPeerConnection = { wrapper: 'gea::host::RTCPeerConnection', construct: 'gea::runtime::hostrtc::createPeer({args})' }
  shims.nativeConstructors!.RTCSessionDescriptionConstructor = 'gea::runtime::hostrtc::createDescription({arg0})'
  shims.embeddedHostClasses!.RTCSessionDescription = { wrapper: 'gea::host::RTCSessionDescriptionObject', construct: 'gea::runtime::hostrtc::createDescription({args})' }
  shims.nativeConstructors!.RTCIceCandidateConstructor = 'gea::runtime::hostrtc::createCandidate({args})'
  shims.embeddedHostClasses!.RTCIceCandidate = { wrapper: 'gea::host::RTCIceCandidate', construct: 'gea::runtime::hostrtc::createCandidate({args})' }
  return shims
}
