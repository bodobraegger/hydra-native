// `pb`, the webRTC patch bay of the web editor (src/lib/patch-bay/pb-live.js): hydra-native-patch-bay
// (src/patch_bay_main.c) does its work with GStreamer, and this object mirrors it for hydra-synth (initStream) and
// sketches (pb.list(), pb.setName()).
// As in the web editor, it exists only with a server (galleryServer, the settings menu), in the room 'iclc'.
import { EventEmitter } from './event-emitter.js'
import { nativeVideo } from './sources.js'

const WEB_EDITOR_ROOM = 'iclc'

export class PatchBay extends EventEmitter {
  constructor () {
    super()
    Object.assign(this, { id: null, nick: null, nicks: [], peers: [], videos: new Map() })
    window.__patchBay = (line) => this.receive(JSON.parse(line))
  }

  // pb.init(stream, { server, room }): the stream is always the output of hydra-native.
  init (stream, { room = WEB_EDITOR_ROOM } = {}) {
    if (!native.patchBayStart(room)) console.warn('pb: cannot start hydra-native-patch-bay, which needs GStreamer at build time')
    this.settings = { server: native.galleryServer(), room }
    window.pb = this
  }

  close () {
    native.patchBayStop()
    for (const video of [...this.videos.values()]) video.close()
    this.id = null
    this.removeAllListeners()
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
        Object.assign(this, {
          id: message.id, streamId: message.streamId, nick: message.nick, nicks: message.nicks, peers: message.peers
        })
        break
      case 'publish':
        native.patchBayPublish(message.on)
        break
      case 'video': this.receiveVideo(message); break
      case 'video closed': this.videos.get(message.nick)?.close(); break
      case 'broadcast': this.emit('broadcast', message.data); break
      case 'message': this.emit('message', message.data); break
      case 'log': (message.error ? console.warn : console.log)(`pb: ${message.text}`); break
    }
  }

  // The first frame of a stream gives 'got video' as in pb-live.js; a new frame size swaps the channel of the
  // same video object, and hydra-synth resizes the texture in tick(). A closed video (the peer left, or the
  // source was cleared) leaves the list, so that the next stream of that peer gives 'got video' again.
  receiveVideo ({ nick, channel, width, height, resized }) {
    const { videos } = this
    const video = videos.get(nick)
    if (!video && resized) return // the source of this stream was cleared
    const next = nativeVideo(native.patchBayVideo(channel, width, height), width, height)
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
      close () {
        this.current.close()
        if (videos.get(nick) === this) videos.delete(nick)
      }
    }
    this.videos.set(nick, created)
    this.emit('got video', nick, created)
  }
}
