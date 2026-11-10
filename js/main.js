// Entry of build/hydra-bundle.js: runs the unchanged hydra-synth renderer on the native shims.
import { onAfterFrame, RecordingContext2D } from './dom-shim.js'
import { pendingFetches } from './network.js'
import Hydra from 'hydra-synth'
import { installSources } from './sources.js'
import beautify from 'js-beautify'
import './editor-actions.js'
import './midi.js'
import './p5.js'
import { startLanguages } from './l10n.js'
import { startExtensions } from './extensions.js'

const canvas = document.createElement('canvas')
;[canvas.width, canvas.height] = native.drawableSize()
document.body.appendChild(canvas)

const hydra = new Hydra({ canvas, enableStreamCapture: false })
// The audio meter of lib/audio.js draws its bars into this recording context, see the overlay code below.
if (hydra.synth.a) {
  hydra.synth.a.ctx = Object.assign(new RecordingContext2D(hydra.synth.a.canvas), { fillStyle: '#DFFFFF', strokeStyle: '#0ff' })
}
// hydra-editor exposes the renderer as hydraSynth; the hydra-extensions look for it there.
window.hydraSynth = hydra
installSources(hydra.s[0].constructor.prototype)
// screencap() of hydra-synth saves through a DOM link; src/main.c writes PNG and code files instead.
hydra.synth.screencap = window.screencap = () => native.screencap()
// vidRecorder of hydra-synth uses MediaRecorder; here src/recorder.c sends the frames to ffmpeg (WebM, VP9).
hydra.synth.vidRecorder = window.vidRecorder = { start: () => native.recordStart(), stop: () => native.recordStop() }

const URL_PATTERN = /^https?:\/\//

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

startLanguages(pendingFetches, native.language, native.l10nUrl)
startExtensions(pendingFetches)

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

// formatCode() of the web editor (src/views/editor/editor.js, the CodeMirror 5 editor that it runs), called by
// src/ui.c: js-beautify with the same options, so the indentation is tabs.
window.__format = (code) => beautify(code, { indent_size: 2, break_chained_methods: true, indent_with_tabs: true })

// Names for the autocomplete of src/ui.c: hydra sources, transforms (also those of setFunction and
// extensions) and the globals of hydra-synth, one per line.
const HYDRA_GLOBALS = ['hush', 'render', 'speed', 'bpm', 'time', 'mouse', 'update', 'afterUpdate', 'setResolution',
  'loadScript', 'screencap', 'vidRecorder', 'P5', 'width', 'height', 'fps', 'setFunction', 'src', 'o0', 'o1', 'o2', 'o3', 's0', 's1', 's2',
  's3', 'a', 'initCam', 'initScreen', 'initVideo', 'initImage', 'initCanvas', 'initArea', 'initWindow', 'initMonitor', 'out',
  'smooth', 'fast', 'ease', 'offset', 'fit', 'setBins', 'setSmooth', 'setScale', 'setCutoff', 'show', 'hide', 'fft']
window.__completionNames = () => [...new Set([...Object.keys(hydra.generator.glslTransforms), ...HYDRA_GLOBALS])].join('\n')
