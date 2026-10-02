import type {} from '@geastack/core'

const empty = new MediaStream()
const copied = new MediaStream(empty)
const tracks = new MediaStream(empty.getTracks())
if (empty.id === copied.id || copied.id === tracks.id) throw new Error('MediaStream constructors reused identity')
if (empty.getTracks().length || copied.getTracks().length || tracks.getTracks().length) throw new Error('expected empty streams')
const peer = new RTCPeerConnection({ iceServers: [{ urls: 'stun:stun.invalid:3478' }] })
const channel = peer.createDataChannel('oai-events')
if (channel.label !== 'oai-events') throw new Error('default data channel options failed')
channel.onmessage = event => {
  if (typeof event.data === 'string') console.log('DOM message ' + event.data)
}
class Listener {
  received = 0
  listen() {
    peer.ontrack = event => {
      this.received++
      if (this.received !== 1) throw new Error('track callback lost lexical this')
      const stream = event.streams[0] || new MediaStream([event.track])
      console.log(stream.id + event.receiver.track.kind + event.transceiver.direction)
      console.log('DOM track ' + this.received)
    }
  }
}
new Listener().listen()
console.log('DOM RTC constructors passed')

async function describePeer(): Promise<void> {
  const offer = await peer.createOffer()
  if (offer.type !== 'offer' || !offer.sdp?.startsWith('v=0')) throw new Error('invalid async offer')
  console.log('DOM async offer passed')
}
void describePeer()
