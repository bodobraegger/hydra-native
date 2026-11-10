// The browser globals that hydra-synth, its lib/audio.js and the user's sketches use,
// backed by the `native` object that src/main.c installs.

globalThis.window = globalThis
globalThis.self = globalThis
globalThis.global = globalThis

console.info = console.debug = (...parts) => console.log(...parts)
// Warnings and errors go to stderr and, during an evaluation, to the error bar of the code overlay.
console.warn = console.error = (...parts) => native.consoleMessage(parts.map(String).join(' '))

window.performance = { now: () => native.now() }
window.location = { href: 'hydra-native://', search: '', hash: '' }

// ---------------------------------------------------------------- events

const listeners = new Map()

function addEventListener (type, listener) {
  if (!listeners.has(type)) listeners.set(type, [])
  listeners.get(type).push(listener)
}

function removeEventListener (type, listener) {
  listeners.set(type, (listeners.get(type) || []).filter((item) => item !== listener))
}

window.addEventListener = addEventListener
window.removeEventListener = removeEventListener

function updateWindowSize () {
  [window.innerWidth, window.innerHeight] = native.drawableSize()
}
updateWindowSize()

// Called by src/main.c for keyboard, mouse and resize events.
window.__dispatchEvent = (type, event) => {
  if (type === 'resize') updateWindowSize()
  Object.assign(event, { type, target: window, preventDefault () {}, stopPropagation () {} })
  for (const listener of (listeners.get(type) || []).slice()) {
    try {
      listener(event)
    } catch (error) {
      console.error(`${type} listener:`, error, error.stack)
    }
  }
}

// ---------------------------------------------------------------- timers and frames
// ponytail: timers fire at frame boundaries, so their resolution is one frame (16 ms at 60 Hz).

let timers = []
let frameCallbacks = []
let nextId = 1
const afterFrameCallbacks = []

function addTimer (callback, delay, args, repeat) {
  const id = nextId++
  const interval = Math.max(Number(delay) || 0, 1)
  timers.push({ id, at: native.now() + interval, interval, callback, args, repeat })
  return id
}

window.setTimeout = (callback, delay, ...args) => addTimer(callback, delay, args, false)
window.setInterval = (callback, delay, ...args) => addTimer(callback, delay, args, true)
window.clearTimeout = window.clearInterval = (id) => { timers = timers.filter((timer) => timer.id !== id) }
window.requestAnimationFrame = (callback) => {
  const id = nextId++
  frameCallbacks.push({ id, callback })
  return id
}
window.cancelAnimationFrame = (id) => { frameCallbacks = frameCallbacks.filter((item) => item.id !== id) }
window.queueMicrotask = (callback) => Promise.resolve().then(callback)

export function onAfterFrame (callback) {
  afterFrameCallbacks.push(callback)
}

function run (label, callback, ...args) {
  try {
    callback(...args)
  } catch (error) {
    console.error(`${label}:`, error, error.stack)
  }
}

// Called by src/main.c once per displayed frame.
window.__frame = (now) => {
  for (const timer of timers.filter((item) => item.at <= now)) {
    if (timer.repeat) timer.at = now + timer.interval
    else timers = timers.filter((item) => item !== timer)
    run('timer', timer.callback, ...timer.args)
  }
  const callbacks = frameCallbacks
  frameCallbacks = []
  for (const { callback } of callbacks) run('requestAnimationFrame', callback, now)
  for (const callback of afterFrameCallbacks) run('after frame', callback, now)
}

// ---------------------------------------------------------------- document

const COLOR_PATTERN = /^#([0-9a-f]{3}|[0-9a-f]{6})$/i

function parseColor (text) {
  const match = COLOR_PATTERN.exec(String(text))
  if (!match) return [1, 1, 1, 1]
  const hex = match[1].length === 3 ? [...match[1]].map((digit) => digit + digit).join('') : match[1]
  return [0, 2, 4].map((offset) => parseInt(hex.slice(offset, offset + 2), 16) / 255).concat(1)
}

// Records the fillRect calls and the horizontal strokes that the hydra audio meter draws.
// The overlay code in main.js draws them with native.fillRects.
class RecordingContext2D {
  constructor (canvas) {
    this.canvas = canvas
    this.fillStyle = '#fff'
    this.strokeStyle = '#fff'
    this.lineWidth = 1
    this.rects = []
    this.path = []
  }

  clearRect () { this.rects = [] }
  fillRect (x, y, width, height) { this.rects.push([x, y, width, height, ...parseColor(this.fillStyle)]) }
  beginPath () { this.path = [] }
  moveTo (x, y) { this.path.push([x, y]) }
  lineTo (x, y) { this.path.push([x, y]) }
  stroke () {
    for (let i = 1; i < this.path.length; i++) {
      const [[x0, y0], [x1, y1]] = [this.path[i - 1], this.path[i]]
      this.rects.push([Math.min(x0, x1), Math.min(y0, y1), Math.max(Math.abs(x1 - x0), 1), Math.max(Math.abs(y1 - y0), 1),
        ...parseColor(this.strokeStyle)])
    }
  }
}

class Element {
  constructor (tagName) {
    this.tagName = tagName.toUpperCase()
    this.style = {}
    this.children = []
    this.parentNode = null
    this.width = 300
    this.height = 150
  }

  appendChild (child) {
    child.parentNode = this
    this.children.push(child)
    return child
  }

  removeChild (child) {
    this.children = this.children.filter((item) => item !== child)
    child.parentNode = null
    return child
  }

  remove () { if (this.parentNode) this.parentNode.removeChild(this) }
  setAttribute (name, value) { this[name] = value }
  addEventListener () {}
  removeEventListener () {}
  getContext (type) {
    if (type !== '2d') return null
    if (!this.context) this.context = new RecordingContext2D(this)
    return this.context
  }

  get clientWidth () { return this.width }
  get clientHeight () { return this.height }
}

window.document = {
  body: new Element('body'),
  head: new Element('head'),
  createElement: (tagName) => new Element(tagName),
  getElementById: () => null,
  querySelector: () => null,
  addEventListener,
  removeEventListener
}

// ---------------------------------------------------------------- media

window.navigator = {
  platform: 'Linux x86_64',
  userAgent: 'hydra-native',
  maxTouchPoints: 0,
  mediaDevices: {
    // lib/audio.js asks for the microphone here; the Meyda shim then reads native audio.
    getUserMedia: () => Promise.resolve({ getTracks: () => [] }),
    enumerateDevices: () => Promise.resolve([])
  }
}

window.AudioContext = class AudioContext {
  createMediaStreamSource () { return {} }
}
