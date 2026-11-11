#!/usr/bin/env node
// The webRTC patch bay of hydra-native (pb, s0.initStream): runs pb-live.js and rtc-patch-bay.js of the web editor
// unchanged in Node.js, with the WebRTC stack of @roamhq/wrtc and the browser globals they use.
//
// usage: node tools/patch-bay.mjs SERVER ROOM SOCKET
// SOCKET is the address of a local socket for the frames: a Unix socket path, or a named pipe (\\.\pipe\...)
// on Windows. Each connection starts with one line that names its channel:
//   output WIDTHxHEIGHT                            hydra-native sends RGBA frames of its output (rows from the bottom)
//   stream-N-WIDTHxHEIGHT                          the helper sends the RGBA frames of a received stream
// stdin, one JSON object per line, from src/patch_bay.c:
//   { type: 'call', method, args }                 a method of pb: initSource, setName, broadcast, list
// stdout, one JSON object per line:
//   { type: 'state', id, nick, nicks, peers }      after each change of the peers or names
//   { type: 'publish', on }                         whether a peer connection wants the output frames
//   { type: 'video', nick, channel, width, height } a received stream, whose frames come on `channel`
//   { type: 'broadcast', data }, { type: 'message', data }, { type: 'log', text }
import fs from 'node:fs'
import net from 'node:net'
import wrtc from '@roamhq/wrtc'

const [server, room, socketAddress] = process.argv.slice(2)
const PUBLISH_CHECK_MS = 500
const NEWLINE = 10
const isUnixSocket = process.platform !== 'win32'
const { RTCVideoSink, RTCVideoSource, i420ToRgba, rgbaToI420 } = wrtc.nonstandard

const send = (message) => process.stdout.write(JSON.stringify(message) + '\n')
process.stdout.on('error', () => process.exit(0)) // hydra-native ended without closing the helper
console.log = console.info = (...parts) => send({ type: 'log', text: parts.map(String).join(' ') })
console.warn = console.error = (...parts) => send({ type: 'log', text: parts.map(String).join(' '), error: true })

// The channels that wait for a connection: name -> function (socket, the bytes after the first line, the second
// word of the first line).
const channels = new Map()
const socketServer = net.createServer((socket) => {
  let head = Buffer.alloc(0)
  const readHead = (chunk) => {
    head = Buffer.concat([head, chunk])
    const end = head.indexOf(NEWLINE)
    if (end < 0) return
    socket.off('data', readHead)
    socket.pause()
    const [name, size] = head.subarray(0, end).toString().split(' ')
    const accept = channels.get(name)
    if (accept) accept(socket, head.subarray(end + 1), size)
    else socket.destroy()
  }
  socket.on('data', readHead)
  socket.on('error', () => {})
})
if (isUnixSocket) fs.rmSync(socketAddress, { force: true })
await new Promise((resolve, reject) => socketServer.once('error', reject).listen(socketAddress, resolve))

// ---------------------------------------------------------------- the browser globals of pb-live.js

const sessionValues = new Map()
globalThis.window = globalThis
globalThis.sessionStorage = {
  getItem: (key) => sessionValues.has(key) ? sessionValues.get(key) : null,
  setItem: (key, value) => sessionValues.set(key, String(value))
}
Object.assign(globalThis, {
  RTCPeerConnection: wrtc.RTCPeerConnection,
  RTCSessionDescription: wrtc.RTCSessionDescription,
  RTCIceCandidate: wrtc.RTCIceCandidate,
  MediaStream: wrtc.MediaStream
})

let receivedStreams = 0

// A <video> element for pb-live.js: its srcObject feeds a channel with RGBA frames, and 'loadedmetadata' comes with
// the first frame. A later change of the frame size (WebRTC starts small) opens a new channel; frames that come
// before hydra-native connects to the channel are dropped.
class VideoElement {
  constructor () { this.listeners = {} }
  addEventListener (type, listener) { (this.listeners[type] ||= []).push(listener) }
  play () {}
  set srcObject (stream) {
    const track = stream.getVideoTracks()[0]
    if (!track) return
    const sink = new RTCVideoSink(track)
    const number = ++receivedStreams
    let output = null, rgba = null
    sink.onframe = ({ frame }) => {
      if (!output || frame.width !== this.videoWidth || frame.height !== this.videoHeight) {
        if (output) output.destroy()
        if (this.channel) channels.delete(this.channel)
        output = null
        this.videoWidth = frame.width
        this.videoHeight = frame.height
        const channel = this.channel = `stream-${number}-${frame.width}x${frame.height}`
        channels.set(channel, (socket) => {
          if (output) output.destroy()
          output = socket
        })
        rgba = new Uint8ClampedArray(frame.width * frame.height * 4)
        if (this.loaded) send({ type: 'video', nick: this.nick, channel, width: frame.width, height: frame.height })
        else {
          this.loaded = true
          for (const listener of this.listeners.loadedmetadata || []) listener()
        }
      }
      // Not connected (yet), or hydra-native reads slower than frames come: drop this frame.
      if (!output || output.destroyed || output.writableNeedDrain) return
      i420ToRgba(frame, { width: frame.width, height: frame.height, data: rgba })
      output.write(Buffer.from(rgba.buffer.slice(0)))
    }
  }
}

globalThis.document = {
  title: '',
  createElement: (name) => name === 'video' ? new VideoElement() : {}
}
globalThis.URL ||= {}

// ---------------------------------------------------------------- the output of hydra-native as a stream

const source = new RTCVideoSource()
const stream = new wrtc.MediaStream([source.createTrack()])
// One connection per publishing period of hydra-native, with the frame size in its first line.
channels.set('output', (input, rest, size) => {
  const [frameWidth, frameHeight] = String(size).split('x').map(Number)
  const frameBytes = frameWidth * frameHeight * 4 || 0
  let pending = Buffer.alloc(0)
  const receive = (chunk) => {
    pending = pending.length ? Buffer.concat([pending, chunk]) : chunk
    while (frameBytes > 0 && pending.length >= frameBytes) {
      // glReadPixels gives the rows from the bottom.
      const rgba = new Uint8ClampedArray(frameBytes), row = frameWidth * 4
      for (let y = 0; y < frameHeight; y++) rgba.set(pending.subarray((frameHeight - 1 - y) * row, (frameHeight - y) * row), y * row)
      pending = pending.subarray(frameBytes)
      const i420 = new Uint8ClampedArray(frameWidth * frameHeight * 3 / 2)
      rgbaToI420({ width: frameWidth, height: frameHeight, data: rgba }, { width: frameWidth, height: frameHeight, data: i420 })
      source.onFrame({ width: frameWidth, height: frameHeight, data: i420 })
    }
  }
  if (rest.length) receive(rest)
  input.on('data', receive)
  input.resume()
})

// ---------------------------------------------------------------- the patch bay of the web editor

const { default: PBLive } = await import('../js/hydra-editor/pb-live.js')
const pb = new PBLive()
// hydra-synth captures its canvas with captureStream(25); this stream gets the frames of hydra-native.
pb.init(stream, { server, room })

// streamId: the socket id, which the other peers list in pb.peers and pass to s0.initStream; pb.id (random) is
// only the default nickname.
const sendState = () => send({ type: 'state', id: pb.id, streamId: pb.signaller.id, nick: pb.nick, nicks: Object.keys(pb.idFromNick),
  peers: Object.keys(pb.peers) })
for (const event of ['ready', 'new peer', 'broadcast', 'connect', 'close']) pb.on(event, () => setImmediate(sendState))
pb.on('broadcast', (data) => send({ type: 'broadcast', data }))
pb.on('message', (data) => send({ type: 'message', data }))
pb.on('got video', (nick, video) => {
  video.nick = nick
  send({ type: 'video', nick, channel: video.channel, width: video.videoWidth, height: video.videoHeight })
})

let publishing = false
setInterval(() => {
  const on = Object.keys(pb.rtcPeers).length > 0
  if (on !== publishing) send({ type: 'publish', on: (publishing = on) })
}, PUBLISH_CHECK_MS)

let input = ''
process.stdin.setEncoding('utf8')
process.stdin.on('data', (chunk) => {
  input += chunk
  let end
  while ((end = input.indexOf('\n')) >= 0) {
    const message = JSON.parse(input.slice(0, end))
    input = input.slice(end + 1)
    if (message.type === 'call' && typeof pb[message.method] === 'function') {
      pb[message.method](...(message.args || []))
      setImmediate(sendState)
    }
  }
})
process.stdin.on('end', () => {
  pb._destroy()
  socketServer.close()
  if (isUnixSocket) fs.rmSync(socketAddress, { force: true })
  process.exit(0)
})
