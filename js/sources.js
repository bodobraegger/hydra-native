// Replaces the DOM media inputs of HydraSource (s0..s3) with native video sources:
// a child process writes raw RGBA frames to a pipe, src/video.c uploads the newest frame.
// src/sandbox.c builds the commands from numbers and URLs (gst-launch-1.0 for cameras on Linux, ffmpeg, yt-dlp and
// build/hydra-native-capture), so that they hold no code of a sketch; only initCommand runs a command of the sketch,
// and the sandbox of shared code turns it off. On macOS and Windows, the cameras come from SDL in the process.

const DEFAULT_WIDTH = 1280
const DEFAULT_HEIGHT = 720
const DEFAULT_WINDOW_DELAY_SECONDS = 3
const YOUTUBE_PATTERN = /^https?:\/\/(www\.)?(youtube\.com|youtu\.be)\//

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
  // the handle of the native video source.
  function initHandle (source, { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}, open) {
    source.clear()
    source.src = nativeVideo(open(width, height), width, height)
    source.tex.destroy()
    source.tex = source.regl.texture({ width, height, ...params })
    source.dynamic = true
    return source
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

  // Loops files and network streams; YouTube links need yt-dlp in PATH and play once.
  sourcePrototype.initVideo = function (url = '', options) {
    const youtube = YOUTUBE_PATTERN.test(url)
    if (!requireProgram('ffmpeg', 'initVideo') || (youtube && !requireProgram('yt-dlp', 'initVideo'))) return
    const kind = youtube ? 'youtube' : 'video'
    return initHandle(this, options, (width, height) => native.mediaOpen(String(url), width, height, kind))
  }

  sourcePrototype.initImage = function (url = '', options) {
    if (!requireProgram('ffmpeg', 'initImage')) return
    return initHandle(this, options, (width, height) => native.mediaOpen(String(url), width, height, 'image'))
  }
}
