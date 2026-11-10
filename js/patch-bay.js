// `pb`, the webRTC patch bay of the web editor (src/lib/patch-bay/pb-live.js): tools/patch-bay.mjs runs that code
// with @roamhq/wrtc, and this object mirrors it for hydra-synth (initStream) and sketches (pb.list(), pb.setName()).
// As in the web editor, it exists only with a server (--gallery-server), in the room 'iclc'.
import { EventEmitter } from 'events'
import { nativeVideo } from './sources.js'

const WEB_EDITOR_ROOM = 'iclc'

const quote = (text) => `'${String(text).replace(/'/g, "'\\''")}'`

export class PatchBay extends EventEmitter {
  constructor () {
    super()
    Object.assign(this, { id: null, nick: null, nicks: [], peers: [], videos: new Map() })
    window.__patchBay = (line) => this.receive(JSON.parse(line))
  }

  // pb.init(stream, { server, room }): the stream is always the output of hydra-native.
  init (stream, { server, room = WEB_EDITOR_ROOM } = {}) {
    if (!native.patchBayStart(server, room)) console.warn('pb: cannot start node tools/patch-bay.mjs')
    this.settings = { server, room }
    window.pb = this
  }

  call (method, ...args) { native.patchBaySend(JSON.stringify({ type: 'call', method, args })) }

  initSource (nick) { this.call('initSource', nick) }
  setName (nick) {
    this.nick = nick
    this.call('setName', nick)
  }

  broadcast (data) { this.call('broadcast', data) }

  list () {
    console.log(this.nicks)
    return this.nicks
  }

  receive (message) {
    switch (message.type) {
      case 'state':
        Object.assign(this, { id: message.id, nick: message.nick, nicks: message.nicks, peers: message.peers })
        break
      case 'publish':
        native.patchBayPublish(message.on)
        break
      case 'video': this.receiveVideo(message); break
      case 'broadcast': this.emit('broadcast', message.data); break
      case 'message': this.emit('message', message.data); break
      case 'log': (message.error ? console.warn : console.log)(`pb: ${message.text}`); break
    }
  }

  // The first frame of a stream gives 'got video' as in pb-live.js; a new frame size swaps the pipe of the same
  // video object, and hydra-synth resizes the texture in tick().
  receiveVideo ({ nick, path, width, height }) {
    const next = nativeVideo(`cat ${quote(path)}`, width, height)
    const video = this.videos.get(nick)
    if (video) {
      video.current.close()
      video.current = next
      return
    }
    const created = {
      current: next,
      get width () { return this.current.width },
      get height () { return this.current.height },
      get videoHandle () { return this.current.videoHandle },
      upload (texture) { return this.current.upload(texture) },
      close () { this.current.close() }
    }
    this.videos.set(nick, created)
    this.emit('got video', nick, created)
  }
}
