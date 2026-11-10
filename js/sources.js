// Replaces the DOM media inputs of HydraSource (s0..s3) with native video sources:
// a child process writes raw RGBA frames to a pipe, src/video.c uploads the newest frame.
// src/main.c builds the commands from numbers and URLs (gst-launch-1.0 for cameras on Linux, ffmpeg, yt-dlp and
// build/hydra-native-capture), so that they hold no code of a sketch; only initCommand runs a command of the sketch,
// and the sandbox of shared code turns it off. On macOS and Windows, the cameras come from SDL in the process.

const DEFAULT_WIDTH = 1280
const DEFAULT_HEIGHT = 720
const DEFAULT_WINDOW_DELAY_SECONDS = 3
const YOUTUBE_PATTERN = /^https?:\/\/(www\.)?(youtube\.com|youtu\.be)\//

// Whether `program` is in PATH; otherwise a warning in the error bar names the call that needs it.
function requireProgram (program, call) {
  if (native.findProgram(program)) return true
  console.warn(`${call} needs ${program} in PATH`)
  return false
}

// A native video source of src/video.c for the texture of a HydraSource, from its handle.
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

  sourcePrototype.clear = function () {
    if (this.src && this.src.close) this.src.close()
    clear.call(this)
  }

  function initHandle (source, open, width, height, params) {
    source.clear()
    source.src = nativeVideo(open(), width, height)
    source.tex = source.regl.texture({ width, height, ...params })
    source.dynamic = true
    return source
  }

  // initCommand(shellCommand, { width, height, ...textureParams }): a command of the sketch that writes frames.
  sourcePrototype.initCommand = function (command, { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}) {
    return initHandle(this, () => native.videoOpen(command, width, height), width, height, params)
  }

  sourcePrototype.initCam = function (index = 0, { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}) {
    const devices = native.cameras()
    if (!devices[index]) return console.warn(`no camera ${index}, found: ${devices.join(', ')}`)
    if (native.platform === 'Linux' && !requireProgram('gst-launch-1.0', 'initCam')) return
    return initHandle(this, () => native.cameraOpen(index, width, height), width, height, params)
  }

  // initScreen() opens the picker of the system (xdg-desktop-portal, SCContentSharingPicker on macOS,
  // GraphicsCapturePicker on Windows), like getDisplayMedia() in the browser: the user selects a window or a monitor. The index of hydra-synth's initScreen(index) is not used.
  sourcePrototype.initScreen = function (index = 0, { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}) {
    return initHandle(this, () => native.captureOpen('picker', width, height), width, height, params)
  }

  // initMonitor(index) records monitor `index` without a dialog (in the order of org.gnome.Mutter.DisplayConfig,
  // SCShareableContent or EnumDisplayMonitors).
  sourcePrototype.initMonitor = function (index = 0, { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}) {
    return initHandle(this, () => native.captureOpen('monitor', index, width, height), width, height, params)
  }

  sourcePrototype.initArea = function (x, y, areaWidth, areaHeight,
    { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}) {
    return initHandle(this, () => native.captureOpen('area', x, y, areaWidth, areaHeight, width, height), width, height,
      params)
  }

  // Records the window that has the focus `delay` seconds after the call.
  sourcePrototype.initWindow = function (delay = DEFAULT_WINDOW_DELAY_SECONDS,
    { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}) {
    console.log(`initWindow: focus the window to record within ${delay} s`)
    return initHandle(this, () => native.captureOpen('window', delay, width, height), width, height, params)
  }

  // Loops files and network streams; YouTube links need yt-dlp in PATH and play once.
  sourcePrototype.initVideo = function (url = '', { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}) {
    const youtube = YOUTUBE_PATTERN.test(url)
    if (!requireProgram('ffmpeg', 'initVideo') || (youtube && !requireProgram('yt-dlp', 'initVideo'))) return
    const kind = youtube ? 'youtube' : 'video'
    return initHandle(this, () => native.mediaOpen(String(url), width, height, kind), width, height, params)
  }

  sourcePrototype.initImage = function (url = '', { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}) {
    if (!requireProgram('ffmpeg', 'initImage')) return
    return initHandle(this, () => native.mediaOpen(String(url), width, height, 'image'), width, height, params)
  }
}
