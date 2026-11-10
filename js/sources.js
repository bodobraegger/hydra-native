// Replaces the DOM media inputs of HydraSource (s0..s3) with native video sources:
// a child process writes raw RGBA frames to a pipe, src/video.c uploads the newest frame.
// The commands use gst-launch-1.0, ffmpeg, tools/portal_screencast.py (the portal picker of initScreen) and
// tools/screencast.py (Mutter ScreenCast without a dialog: initMonitor, initArea, initWindow).

const DEFAULT_WIDTH = 1280
const DEFAULT_HEIGHT = 720
const DEFAULT_WINDOW_DELAY_SECONDS = 3
const MAX_VIDEO_DEVICES = 64
const YOUTUBE_PATTERN = /^https?:\/\/(www\.)?(youtube\.com|youtu\.be)\//

const quote = (text) => `'${String(text).replace(/'/g, "'\\''")}'`
const rawVideoCaps = (width, height) => `video/x-raw,format=RGBA,width=${width},height=${height}`
const ffmpegOutput = (width, height) => `-vf scale=${width}:${height} -pix_fmt rgba -f rawvideo -`
const screencast = (...args) => `python3 ${quote(native.directory + '/../tools/screencast.py')} ${args.join(' ')}`

// Capture devices in the order of their /dev/video number. Metadata nodes (index 1) are skipped.
function cameraDevices () {
  const devices = []
  for (let number = 0; number < MAX_VIDEO_DEVICES; number++) {
    try {
      if (native.readFile(`/sys/class/video4linux/video${number}/index`).trim() === '0') devices.push(`/dev/video${number}`)
    } catch {}
  }
  return devices
}

// A native video source for the texture of a HydraSource: the RGBA frames that `command` writes to stdout.
export function nativeVideo (command, width, height) {
  const handle = native.videoOpen(command, width, height)
  return {
    width,
    height,
    command,
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

  // initCommand(shellCommand, { width, height, ...textureParams }) is the base of all other inputs.
  sourcePrototype.initCommand = function (command, { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}) {
    this.clear()
    this.src = nativeVideo(command, width, height)
    this.tex = this.regl.texture({ width, height, ...params })
    this.dynamic = true
    return this
  }

  sourcePrototype.initCam = function (index = 0, { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}) {
    const device = cameraDevices()[index]
    if (!device) return console.warn(`no camera ${index}, found: ${cameraDevices().join(' ')}`)
    return this.initCommand(`gst-launch-1.0 -q v4l2src device=${device} ! decodebin ! videoconvert ! videoscale ! ` +
      `${rawVideoCaps(width, height)} ! fdsink fd=1 sync=false`, { width, height, ...params })
  }

  // initScreen() opens the picker of xdg-desktop-portal, like getDisplayMedia() in the browser:
  // the user selects a window or a monitor. The index of hydra-synth's initScreen(index) is not used.
  sourcePrototype.initScreen = function (index = 0, { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}) {
    return this.initCommand(`python3 ${quote(native.directory + '/../tools/portal_screencast.py')} ${width} ${height}`,
      { width, height, ...params })
  }

  // initMonitor(index) records monitor `index` of org.gnome.Mutter.DisplayConfig without a dialog.
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
    const input = YOUTUBE_PATTERN.test(url)
      ? `yt-dlp -q -f 'best[height<=${height}]/best' -o - ${quote(url)} | ffmpeg -hide_banner -loglevel error -re -i -`
      : `ffmpeg -hide_banner -loglevel error -re -stream_loop -1 -i ${quote(url.replace(/^file:\/\//, ''))}`
    return this.initCommand(`${input} ${ffmpegOutput(width, height)}`, { width, height, ...params })
  }

  sourcePrototype.initImage = function (url = '', { width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, ...params } = {}) {
    return this.initCommand(`ffmpeg -hide_banner -loglevel error -i ${quote(url.replace(/^file:\/\//, ''))} ` +
      `-frames:v 1 ${ffmpegOutput(width, height)}`, { width, height, ...params })
  }
}
