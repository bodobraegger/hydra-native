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

// Loads local files only (absolute paths or file:// URLs), like a <script> tag in global scope.
window.loadScript = (url = '') => {
  const path = url.replace(/^file:\/\//, '')
  try {
    globalThis.eval(native.readFile(path))
    console.log(`loaded script ${path}`)
  } catch (error) {
    console.error(`error loading script ${path}:`, error, error.stack || '')
  }
  return Promise.resolve()
}

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
