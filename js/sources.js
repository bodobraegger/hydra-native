// Replaces the DOM media inputs of HydraSource (s0..s3) with native video sources:
// a child process writes raw RGBA frames to a pipe, src/video.c uploads the newest frame.
// The commands use gst-launch-1.0 (cameras on Linux), ffmpeg and build/hydra-native-capture (src/capture.h: the
// system picker of initScreen, and initMonitor, initArea and initWindow without a dialog). On macOS and Windows,
// the cameras come from SDL in the process, and the commands run in cmd.exe on Windows.

const DEFAULT_WIDTH = 1280
const DEFAULT_HEIGHT = 720
const DEFAULT_WINDOW_DELAY_SECONDS = 3
const MAX_VIDEO_DEVICES = 64
const YOUTUBE_PATTERN = /^https?:\/\/(www\.)?(youtube\.com|youtu\.be)\//

const WINDOWS = native.platform === 'Windows'
// A word for the shell of src/process.h: /bin/sh, or cmd.exe, where a file name cannot contain a quote.
const quote = WINDOWS ? (text) => `"${String(text)}"` : (text) => `'${String(text).replace(/'/g, "'\\''")}'`
const rawVideoCaps = (width, height) => `video/x-raw,format=RGBA,width=${width},height=${height}`
const ffmpegOutput = (width, height) => `-vf scale=${width}:${height} -pix_fmt rgba -f rawvideo -`
const CAPTURE_PROGRAM = WINDOWS ? '\\hydra-native-capture.exe' : '/hydra-native-capture'
const screencast = (...args) => `${quote(native.directory + CAPTURE_PROGRAM)} ${args.join(' ')}`

// Whether `program` is in PATH; otherwise a warning in the error bar names the call that needs it.
function requireProgram (program, call) {
  if (native.findProgram(program)) return true
  console.warn(`${call} needs ${program} in PATH`)
  return false
}

// Capture devices: on Linux in the order of their /dev/video number (metadata nodes, index 1, are skipped),
// elsewhere the names of the SDL cameras.
function cameraDevices () {
  if (native.platform !== 'Linux') return native.cameras()
  const devices = []
  for (let number = 0; number < MAX_VIDEO_DEVICES; number++) {
    try {
      if (native.readFile(`/sys/class/video4linux/video${number}/index`).trim() === '0') devices.push(`/dev/video${number}`)
    } catch {}
  }
  return devices
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

  // initCommand(shellCommand, { width, height, ...textureParams }) is the base of all other inputs.
  sourcePrototype.initCommand = function (command, { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}) {
    return initHandle(this, () => native.videoOpen(command, width, height), width, height, params)
  }

  sourcePrototype.initCam = function (index = 0, { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}) {
    const devices = cameraDevices()
    if (!devices[index]) return console.warn(`no camera ${index}, found: ${devices.join(', ')}`)
    if (native.platform !== 'Linux') {
      return initHandle(this, () => native.cameraOpen(index, width, height), width, height, params)
    }
    if (!requireProgram('gst-launch-1.0', 'initCam')) return
    const device = devices[index]
    return this.initCommand(`gst-launch-1.0 -q v4l2src device=${device} ! decodebin ! videoconvert ! videoscale ! ` +
      `${rawVideoCaps(width, height)} ! fdsink fd=1 sync=false`, { width, height, ...params })
  }

  // initScreen() opens the picker of the system (xdg-desktop-portal, SCContentSharingPicker on macOS,
  // GraphicsCapturePicker on Windows), like getDisplayMedia() in the browser: the user selects a window or a monitor. The index of hydra-synth's initScreen(index) is not used.
  sourcePrototype.initScreen = function (index = 0, { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}) {
    return this.initCommand(screencast('picker', width, height),
      { width, height, ...params })
  }

  // initMonitor(index) records monitor `index` without a dialog (in the order of org.gnome.Mutter.DisplayConfig,
  // SCShareableContent or EnumDisplayMonitors).
  sourcePrototype.initMonitor = function (index = 0, { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}) {
    return this.initCommand(screencast('monitor', index, width, height), { width, height, ...params })
  }

  sourcePrototype.initArea = function (x, y, areaWidth, areaHeight,
    { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}) {
    return this.initCommand(screencast('area', x, y, areaWidth, areaHeight, width, height), { width, height, ...params })
  }

  // Records the window that has the focus `delay` seconds after the call.
  sourcePrototype.initWindow = function (delay = DEFAULT_WINDOW_DELAY_SECONDS,
    { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}) {
    console.log(`initWindow: focus the window to record within ${delay} s`)
    return this.initCommand(screencast('window', delay, width, height), { width, height, ...params })
  }

  // Loops files and network streams; YouTube links need yt-dlp in PATH and play once.
  sourcePrototype.initVideo = function (url = '', { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}) {
    const youtube = YOUTUBE_PATTERN.test(url)
    if (!requireProgram('ffmpeg', 'initVideo') || (youtube && !requireProgram('yt-dlp', 'initVideo'))) return
    const input = youtube
      ? `yt-dlp -q -f ${quote(`best[height<=${height}]/best`)} -o - ${quote(url)} | ffmpeg -hide_banner -loglevel error -re -i -`
      : `ffmpeg -hide_banner -loglevel error -re -stream_loop -1 -i ${quote(url.replace(/^file:\/\//, ''))}`
    return this.initCommand(`${input} ${ffmpegOutput(width, height)}`, { width, height, ...params })
  }

  sourcePrototype.initImage = function (url = '', { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}) {
    if (!requireProgram('ffmpeg', 'initImage')) return
    return this.initCommand(`ffmpeg -hide_banner -loglevel error -i ${quote(url.replace(/^file:\/\//, ''))} ` +
      `-frames:v 1 ${ffmpegOutput(width, height)}`, { width, height, ...params })
  }
}
