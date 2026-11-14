// Replaces the DOM media inputs of HydraSource (s0..s3) with native video sources:
// a child process writes raw RGBA frames to a pipe, src/video.c uploads the newest frame.
// src/sandbox.c builds the commands from numbers and URLs (gst-launch-1.0 for cameras on Linux, ffmpeg, yt-dlp and
// build/hydra-native-capture), so that they hold no code of a sketch; only initCommand runs a command of the sketch,
// and the sandbox of shared code turns it off. On macOS and Windows, the cameras come from SDL in the process.
import { EventTarget } from './element.js'
import { pendingFetches } from './network.js'

const DEFAULT_WIDTH = 1280
const DEFAULT_HEIGHT = 720
const DEFAULT_WINDOW_DELAY_SECONDS = 3
const YOUTUBE_PATTERN = /^https?:\/\/(www\.)?(youtube\.com|youtu\.be)\//
const ABSOLUTE_PATTERN = /^([a-z]+:\/\/|[/\\]|[A-Za-z]:)/ // URLs and absolute paths, also of Windows
const MIN_PLAYBACK_RATE = 0.25
const MAX_PLAYBACK_RATE = 4

// A missing program gives a warning in the error bar.
function requireProgram (program, call) {
  if (native.findProgram(program)) return true
  console.warn(`${call} needs ${program} in PATH`)
  return false
}

export function nativeVideo (handle, width, height) {
  return {
    width,
    height,
    videoHandle: handle,
    upload: (texture) => native.videoUpload(handle, texture.id),
    close: () => native.videoClose(handle),
    get running () { return native.videoRunning(handle) }
  }
}

// A relative path is relative to the folder of the running file or of the open sketch, as a URL is relative to its
// page in the browser.
export const scriptRelative = (url) => ABSOLUTE_PATTERN.test(url) ? url : native.scriptFolder() + url

// s0.src of initVideo, like the <video> element of the web editor. A seek, play() and a new playbackRate or loop
// start a new ffmpeg at the position, and pause() stops ffmpeg; the texture keeps the last frame meanwhile.
// currentTime counts the frames from the start position with the frame rate of ffprobe.
class VideoElement extends EventTarget {
  constructor (handle, url, width, height, loop) {
    super()
    this.handle = handle
    this.url = url
    this.width = width
    this.height = height
    this.looping = loop
    this.start = 0
    this.rate = 1
    this.paused = false
    this.duration = NaN
    this.frameRate = NaN
    const probe = native.mediaProbe(url)
    if (probe >= 0) {
      pendingFetches.set(probe, (ok, text) => {
        for (const line of text.trim().split('\n')) {
          const [numerator, denominator] = line.split('/')
          if (denominator) this.frameRate = numerator / denominator
          else this.duration = parseFloat(line)
        }
      })
    }
  }

  get currentTime () {
    const time = this.handle < 0 || !(this.frameRate > 0) ? this.start
      : this.start + native.videoFrames(this.handle) / this.frameRate
    if (!(this.duration > 0)) return time
    return this.looping ? time % this.duration : Math.min(time, this.duration)
  }

  // A new ffmpeg seeks with -ss before its input; a paused video keeps the position for play().
  set currentTime (seconds) {
    this.start = Math.max(0, Number(seconds) || 0)
    if (this.looping && this.duration > 0) this.start %= this.duration
    if (this.paused) return
    const previous = this.handle
    this.handle = native.mediaOpen(this.url, this.width, this.height, 'video', this.start, this.rate, this.looping)
    native.videoClose(previous)
  }

  get playbackRate () { return this.rate }

  set playbackRate (rate) {
    const position = this.currentTime
    this.rate = Math.min(MAX_PLAYBACK_RATE, Math.max(MIN_PLAYBACK_RATE, Number(rate) || 1))
    this.currentTime = position
  }

  get loop () { return this.looping }

  set loop (loop) {
    const position = this.currentTime
    this.looping = Boolean(loop)
    this.currentTime = position
  }

  play () {
    if (this.paused) {
      if (this.start >= this.duration) this.start = 0
      this.paused = false
      this.currentTime = this.start
    }
    return Promise.resolve()
  }

  pause () {
    if (this.paused) return
    this.start = this.currentTime
    this.paused = true
    this.close()
  }

  upload (texture) {
    // Read before the upload: when ffmpeg has ended, the reader has delivered its last frame already.
    const running = native.videoRunning(this.handle)
    native.videoUpload(this.handle, texture.id)
    if (this.paused || running) return
    this.pause()
    if (!this.looping) this.dispatchEvent({ type: 'ended', target: this, currentTarget: this })
  }

  close () {
    native.videoClose(this.handle)
    this.handle = -1
  }
}

// A render to file starts each playing video again at its position, so that its sound in the render (src/render.c)
// starts there.
export function restartVideos (sources) {
  for (const { src } of sources) if (src instanceof VideoElement) src.currentTime = src.currentTime
}

export function installSources (sourcePrototype) {
  const clear = sourcePrototype.clear

  // hydra-synth only replaces the texture; each init call would leave a texture of the capture size until the next
  // garbage collection (js/regl-shim.js).
  sourcePrototype.clear = function () {
    if (this.src && this.src.close) this.src.close()
    this.tex.destroy()
    clear.call(this)
  }

  // `options` of an init call: the capture size and the texture options of regl. `open(width, height)` gives
  // the handle of the native video source, and `createSource` its object in `source.src`.
  function initHandle (source, { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}, open,
    createSource = nativeVideo) {
    source.clear()
    source.src = createSource(open(width, height), width, height)
    source.tex.destroy()
    source.tex = source.regl.texture({ width, height, ...params })
    source.dynamic = true
    return source
  }

  // initStream(id, { fps }): the most frames per second of that stream, in place of the setting "stream max rate".
  // The other params are the texture options of hydra-synth.
  const initStream = sourcePrototype.initStream
  sourcePrototype.initStream = function (streamName, { fps, ...params } = {}) {
    if (streamName && this.pb) this.pb.setMaxRate(streamName, fps)
    return initStream.call(this, streamName, params)
  }

  // The shell command of the sketch writes raw RGBA frames to its stdout.
  sourcePrototype.initCommand = function (command, options) {
    return initHandle(this, options, (width, height) => native.videoOpen(command, width, height))
  }

  sourcePrototype.initCam = function (index = 0, options) {
    const devices = native.cameras()
    if (!devices[index]) return console.warn(`no camera ${index}, found: ${devices.join(', ')}`)
    if (native.platform === 'Linux' && !requireProgram('gst-launch-1.0', 'initCam')) return
    return initHandle(this, options, (width, height) => native.cameraOpen(index, width, height))
  }

  // initScreen() opens the picker of the system (xdg-desktop-portal, SCContentSharingPicker on macOS,
  // GraphicsCapturePicker on Windows), like getDisplayMedia() in the browser. The index of hydra-synth's
  // initScreen(index) is not used.
  sourcePrototype.initScreen = function (index = 0, options) {
    return initHandle(this, options, (width, height) => native.captureOpen('picker', width, height))
  }

  // Monitor `index` without a dialog, in the order of org.gnome.Mutter.DisplayConfig, SCShareableContent or
  // EnumDisplayMonitors.
  sourcePrototype.initMonitor = function (index = 0, options) {
    return initHandle(this, options, (width, height) => native.captureOpen('monitor', index, width, height))
  }

  sourcePrototype.initArea = function (x, y, areaWidth, areaHeight, options) {
    return initHandle(this, options,
      (width, height) => native.captureOpen('area', x, y, areaWidth, areaHeight, width, height))
  }

  // Records the window that has the focus `delay` seconds after the call.
  sourcePrototype.initWindow = function (delay = DEFAULT_WINDOW_DELAY_SECONDS, options) {
    console.log(`initWindow: focus the window to record within ${delay} s`)
    return initHandle(this, options, (width, height) => native.captureOpen('window', delay, width, height))
  }

  // Files and network streams play in a loop unless `loop` is false, with the controls of VideoElement; YouTube
  // links need yt-dlp in PATH, play once and have no controls.
  sourcePrototype.initVideo = function (url = '', { loop = true, ...options } = {}) {
    url = scriptRelative(String(url))
    if (YOUTUBE_PATTERN.test(url)) {
      if (!requireProgram('ffmpeg', 'initVideo') || !requireProgram('yt-dlp', 'initVideo')) return
      return initHandle(this, options, (width, height) => native.mediaOpen(url, width, height, 'youtube'))
    }
    if (!requireProgram('ffmpeg', 'initVideo')) return
    return initHandle(this, options, (width, height) => native.mediaOpen(url, width, height, 'video', 0, 1, loop),
      (handle, width, height) => new VideoElement(handle, url, width, height, Boolean(loop)))
  }

  sourcePrototype.initImage = function (url = '', options) {
    if (!requireProgram('ffmpeg', 'initImage')) return
    const path = scriptRelative(String(url))
    return initHandle(this, options, (width, height) => native.mediaOpen(path, width, height, 'image'))
  }
}
