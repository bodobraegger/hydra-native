#!/usr/bin/env node
// The webRTC patch bay of hydra-native (pb, s0.initStream): runs pb-live.js and rtc-patch-bay.js of the web editor
// unchanged in Node.js, with the WebRTC stack of @roamhq/wrtc and the browser globals they use.
//
// usage: node tools/patch-bay.mjs SERVER ROOM DIRECTORY
// stdin, one JSON object per line, from src/patch_bay.c:
//   { type: 'size', width, height }               the size of the output frames that follow in DIRECTORY/output
//   { type: 'call', method, args }                 a method of pb: initSource, setName, broadcast, list
// stdout, one JSON object per line:
//   { type: 'state', id, nick, nicks, peers }      after each change of the peers or names
//   { type: 'publish', on }                         whether a peer connection wants the output frames
//   { type: 'video', nick, path, width, height }   RGBA frames of a received stream in the FIFO at `path`
//   { type: 'broadcast', data }, { type: 'message', data }, { type: 'log', text }
import fs from 'node:fs'
import path from 'node:path'
import { execFileSync } from 'node:child_process'
import wrtc from '@roamhq/wrtc'

const [server, room, directory] = process.argv.slice(2)
fs.mkdirSync(directory, { recursive: true })
const PUBLISH_CHECK_MS = 500
const { RTCVideoSink, RTCVideoSource, i420ToRgba, rgbaToI420 } = wrtc.nonstandard

const send = (message) => process.stdout.write(JSON.stringify(message) + '\n')
console.log = console.info = (...parts) => send({ type: 'log', text: parts.map(String).join(' ') })
console.warn = console.error = (...parts) => send({ type: 'log', text: parts.map(String).join(' '), error: true })

const makeFifo = (name) => {
  const file = path.join(directory, name)
  fs.rmSync(file, { force: true })
  execFileSync('mkfifo', [file])
  return file
}

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

// A <video> element for pb-live.js: its srcObject feeds a FIFO with RGBA frames, and 'loadedmetadata' comes with
// the first frame. A later change of the frame size (WebRTC starts small) opens a new FIFO.
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
        this.videoWidth = frame.width
        this.videoHeight = frame.height
        this.path = makeFifo(`stream-${number}-${frame.width}x${frame.height}`)
        output = fs.createWriteStream(this.path)
        output.on('error', () => {})
        rgba = new Uint8ClampedArray(frame.width * frame.height * 4)
        if (this.loaded) send({ type: 'video', nick: this.nick, path: this.path, width: frame.width, height: frame.height })
        else {
          this.loaded = true
          for (const listener of this.listeners.loadedmetadata || []) listener()
        }
      }
      if (output.writableNeedDrain) return // hydra-native reads slower than frames come: drop this one
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
let frameWidth = 0, frameHeight = 0, pending = Buffer.alloc(0)

function readOutput (file) {
  const input = fs.createReadStream(file, { highWaterMark: 1 << 20 })
  input.on('data', (chunk) => {
    pending = pending.length ? Buffer.concat([pending, chunk]) : chunk
    const size = frameWidth * frameHeight * 4
    while (size > 0 && pending.length >= size) {
      // glReadPixels gives the rows from the bottom.
      const rgba = new Uint8ClampedArray(size), row = frameWidth * 4
      for (let y = 0; y < frameHeight; y++) rgba.set(pending.subarray((frameHeight - 1 - y) * row, (frameHeight - y) * row), y * row)
      pending = pending.subarray(size)
      const i420 = new Uint8ClampedArray(frameWidth * frameHeight * 3 / 2)
      rgbaToI420({ width: frameWidth, height: frameHeight, data: rgba }, { width: frameWidth, height: frameHeight, data: i420 })
      source.onFrame({ width: frameWidth, height: frameHeight, data: i420 })
    }
  })
  input.on('end', () => readOutput(file)) // hydra-native closed the FIFO between two publishing periods
}

// ---------------------------------------------------------------- the patch bay of the web editor

const { default: PBLive } = await import('../js/hydra-editor/pb-live.js')
const pb = new PBLive()
// hydra-synth captures its canvas with captureStream(25); this stream gets the frames of hydra-native.
pb.init(stream, { server, room })

const sendState = () => send({ type: 'state', id: pb.id, nick: pb.nick, nicks: Object.keys(pb.idFromNick), peers: Object.keys(pb.peers) })
for (const event of ['ready', 'new peer', 'broadcast', 'connect', 'close']) pb.on(event, () => setImmediate(sendState))
pb.on('broadcast', (data) => send({ type: 'broadcast', data }))
pb.on('message', (data) => send({ type: 'message', data }))
pb.on('got video', (nick, video) => {
  video.nick = nick
  send({ type: 'video', nick, path: video.path, width: video.videoWidth, height: video.videoHeight })
})

let publishing = false
setInterval(() => {
  const on = Object.keys(pb.rtcPeers).length > 0
  if (on !== publishing) send({ type: 'publish', on: (publishing = on) })
}, PUBLISH_CHECK_MS)

readOutput(makeFifo('output'))

let input = ''
process.stdin.setEncoding('utf8')
process.stdin.on('data', (chunk) => {
  input += chunk
  let end
  while ((end = input.indexOf('\n')) >= 0) {
    const message = JSON.parse(input.slice(0, end))
    input = input.slice(end + 1)
    if (message.type === 'size') {
      frameWidth = message.width
      frameHeight = message.height
      pending = Buffer.alloc(0)
    } else if (message.type === 'call' && typeof pb[message.method] === 'function') {
      pb[message.method](...(message.args || []))
      setImmediate(sendState)
    }
  }
})
process.stdin.on('end', () => {
  pb._destroy()
  fs.rmSync(directory, { recursive: true, force: true })
  process.exit(0)
})
