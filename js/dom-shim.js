import { CanvasElement, CanvasGradient, CanvasPattern, ImageData, ImageElement, Path2D } from './canvas.js'
import { Element } from './element.js'

// The browser globals that hydra-synth, its lib/audio.js and the user's sketches use,
// backed by the `native` object that src/main.c installs.

globalThis.window = globalThis
globalThis.self = globalThis
globalThis.global = globalThis

console.info = console.debug = (...parts) => console.log(...parts)
// Warnings and errors go to stderr and, during an evaluation, to the error bar of the code overlay.
console.warn = console.error = (...parts) => native.consoleMessage(parts.map(String).join(' '))

window.performance = { now: () => native.now() }
window.location = { href: 'hydra-native://local/', protocol: 'hydra-native:', host: 'local', hostname: 'local', pathname: '/', search: '', hash: '' }

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
// Timers run in time order between frames: src/main.c calls __runTimers while it waits for the next frame,
// so a timer fires within about a millisecond of its time, unless a frame is just rendering.

const MINIMUM_REPEAT_MS = 1
const MAX_TIMER_RUNS = 1000 // per call, so that a zero-delay loop cannot block the frames

let timers = []
let frameCallbacks = []
let nextId = 1
const afterFrameCallbacks = []

function addTimer (callback, delay, args, repeat) {
  const id = nextId++
  const interval = Math.max(Number(delay) || 0, 0)
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

// Runs the due timers, the earliest first; returns the time of the next timer, or -1.
window.__runTimers = (now) => {
  for (let runs = 0; runs < MAX_TIMER_RUNS; runs++) {
    let due = null
    for (const timer of timers) if (timer.at <= now && (!due || timer.at < due.at)) due = timer
    if (!due) break
    if (due.repeat) due.at = Math.max(due.at + Math.max(due.interval, MINIMUM_REPEAT_MS), now)
    else timers = timers.filter((item) => item !== due)
    run('timer', due.callback, ...due.args)
    now = native.now()
  }
  return timers.reduce((next, timer) => (next < 0 || timer.at < next ? timer.at : next), -1)
}

// Called by src/main.c once per displayed frame.
window.__frame = (now) => {
  window.__runTimers(now)
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
export class RecordingContext2D {
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

const documentElement = new Element('html')
window.document = {
  readyState: 'complete',
  documentElement,
  body: documentElement.appendChild(new Element('body')),
  head: documentElement.appendChild(new Element('head')),
  createElement: (tagName) => {
    const name = String(tagName).toLowerCase()
    return name === 'canvas' ? new CanvasElement() : name === 'img' ? new ImageElement() : new Element(tagName)
  },
  createElementNS: (namespace, tagName) => window.document.createElement(tagName),
  createTextNode: (text) => ({ nodeType: 3, textContent: String(text) }),
  getElementById: (id) => documentElement.getElementById(id),
  getElementsByTagName: (name) => documentElement.getElementsByTagName(name),
  getElementsByClassName: (name) => documentElement.getElementsByClassName(name),
  querySelector: (selector) => documentElement.querySelector(selector),
  querySelectorAll: (selector) => documentElement.querySelectorAll(selector),
  hasFocus: () => true,
  addEventListener,
  removeEventListener
}
window.devicePixelRatio = 1
window.screen = { get width () { return window.innerWidth }, get height () { return window.innerHeight } }
window.scrollX = window.scrollY = window.pageXOffset = window.pageYOffset = 0
window.matchMedia = () => ({ matches: false, addListener () {}, removeListener () {}, addEventListener () {} })
window.getComputedStyle = (element) => ({ ...element.style, getPropertyValue: (name) => element.style[name] || '' })

// ---------------------------------------------------------------- media

window.navigator = {
  platform: 'Linux x86_64',
  userAgent: 'hydra-native',
  language: 'en-US',
  languages: ['en-US'],
  maxTouchPoints: 0,
  mediaDevices: {
    // lib/audio.js asks for the microphone here; the Meyda shim then reads native audio.
    getUserMedia: () => Promise.resolve({ getTracks: () => [] }),
    enumerateDevices: () => Promise.resolve([])
  }
}

window.Path2D = Path2D
window.Image = ImageElement
window.ImageData = ImageData
window.CanvasGradient = CanvasGradient
window.CanvasPattern = CanvasPattern

window.AudioContext = class AudioContext {
  createMediaStreamSource () { return {} }
}
