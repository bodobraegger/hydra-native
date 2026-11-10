// Entry of build/hydra-bundle.js: runs the unchanged hydra-synth renderer on the native shims.
import { onAfterFrame } from './dom-shim.js'
import Hydra from 'hydra-synth'
import { installSources } from './sources.js'

const canvas = document.createElement('canvas')
;[canvas.width, canvas.height] = native.drawableSize()
document.body.appendChild(canvas)

const hydra = new Hydra({ canvas, enableStreamCapture: false })
// hydra-editor exposes the renderer as hydraSynth; the hydra-extensions look for it there.
window.hydraSynth = hydra
installSources(hydra.s[0].constructor.prototype)

const URL_PATTERN = /^https?:\/\//
const pendingFetches = new Map()

// src/main.c calls this when a download of native.fetchStart() ends.
window.__fetchDone = (id, ok, text) => {
  const callback = pendingFetches.get(id)
  pendingFetches.delete(id)
  if (callback) callback(ok, text)
}

function runScript (code, url) {
  try {
    globalThis.eval(code)
    console.log(`loaded script ${url}`)
  } catch (error) {
    console.error(`error loading script ${url}:`, error, error.stack || '')
  }
}

// Like hydra-synth's loadScript (a <script> tag): runs the file in the global scope and always resolves.
// Paths, file:// URLs and http(s) URLs work; downloads use curl.
window.loadScript = (url = '') => new Promise((resolve) => {
  if (!URL_PATTERN.test(url)) {
    const path = url.replace(/^file:\/\//, '')
    try {
      runScript(native.readFile(path), path)
    } catch (error) {
      console.error(`error loading script ${path}:`, error)
    }
    return resolve()
  }
  pendingFetches.set(native.fetchStart(url), (ok, text) => {
    if (ok) runScript(text, url)
    else console.error(`error loading script ${url}`)
    resolve()
  })
})

window.addEventListener('resize', () => {
  const [width, height] = native.drawableSize()
  if (width !== canvas.width || height !== canvas.height) hydra.setResolution(width, height)
})

// Draws the audio meter of `a.show()` (lib/audio.js draws into a 2D canvas) in the bottom right corner.
onAfterFrame(() => {
  const audio = hydra.synth.a
  if (!audio || !audio.isDrawing || !audio.ctx) return
  const [windowWidth, windowHeight] = native.drawableSize()
  const left = windowWidth - audio.canvas.width
  const top = windowHeight - audio.canvas.height
  native.fillRects(audio.ctx.rects.flatMap(([x, y, width, height, ...color]) => [left + x, top + y, width, height, ...color]))
})
