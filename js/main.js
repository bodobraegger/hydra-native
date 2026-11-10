// Entry of build/hydra-bundle.js: runs the unchanged hydra-synth renderer on the native shims.
import { onAfterFrame, RecordingContext2D } from './dom-shim.js'
import { pendingFetches } from './network.js'
import { apiSignatureLines } from './api-signatures.js'
import { PatchBay } from './patch-bay.js'
import Hydra from 'hydra-synth'
import { installSources } from './sources.js'
import './editor-actions.js'
import './midi.js'
import './p5.js'
import { startLanguages } from './l10n.js'
import { startExtensions } from './extensions.js'

const canvas = document.createElement('canvas')
;[canvas.width, canvas.height] = native.drawableSize()
document.body.appendChild(canvas)

const hydra = new Hydra({ canvas, enableStreamCapture: false })

// As in the web editor (src/views/Hydra.js): with a server, pb joins the room 'iclc' and publishes the output,
// and hydra-synth uses it for initStream; without one, pb is undefined. The settings menu changes the server
// while the program runs (src/ui.c, set_server).
let patchBay = null
window.__setPatchBayServer = (server) => {
  if (patchBay) patchBay.close()
  patchBay = server ? new PatchBay() : null
  if (patchBay) patchBay.init(null, { server })
  else delete window.pb
  hydra.pb = patchBay
  for (const source of hydra.s) source.pb = patchBay
  native.setGalleryServer(server || null)
  native.galleryServer = server || null
  return ''
}
// The stream id for the settings panel and Ctrl+Alt+I; others connect with s0.initStream('<stream id>').
window.__patchBayStreamId = () => patchBay && patchBay.streamId ? patchBay.streamId : ''
if (native.galleryServer) window.__setPatchBayServer(native.galleryServer)
// The audio meter of lib/audio.js draws its bars into this recording context, see the overlay code below.
if (hydra.synth.a) {
  hydra.synth.a.ctx = Object.assign(new RecordingContext2D(hydra.synth.a.canvas), { fillStyle: '#DFFFFF', strokeStyle: '#0ff' })
}
// hydra-editor exposes the renderer as hydraSynth; the hydra-extensions look for it there.
window.hydraSynth = hydra
installSources(hydra.s[0].constructor.prototype)

// hydra-synth replaces the pass of an output at once (Output.render). Here the old pass keeps drawing until the
// driver has compiled the new one (src/programs.c), so that an eval does not stop the frames. The newest pass wins.
const outputPrototype = hydra.o[0].constructor.prototype
const renderOutput = outputPrototype.render
outputPrototype.render = function (passes) {
  const current = this.draw
  renderOutput.call(this, passes)
  this.pendingDraw = this.draw
  this.draw = current
}
const tickOutput = outputPrototype.tick
outputPrototype.tick = function (props) {
  if (this.pendingDraw) {
    try {
      if (this.pendingDraw.ready()) [this.draw, this.pendingDraw] = [this.pendingDraw, null]
    } catch (error) {
      this.pendingDraw = null
      native.showError(`shader could not compile ${error}`)
    }
  }
  tickOutput.call(this, props)
}
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

// Names for the autocomplete of src/ui.c: hydra sources, transforms (also those of setFunction and
// extensions) and the globals of hydra-synth, one per line.
const HYDRA_GLOBALS = ['hush', 'render', 'speed', 'bpm', 'time', 'mouse', 'update', 'afterUpdate', 'setResolution',
  'loadScript', 'screencap', 'vidRecorder', 'P5', 'width', 'height', 'fps', 'setFunction', 'src', 'o0', 'o1', 'o2', 'o3', 's0', 's1', 's2',
  's3', 'a', 'initCam', 'initScreen', 'initVideo', 'initImage', 'initCanvas', 'initArea', 'initWindow', 'initMonitor', 'out',
  'smooth', 'fast', 'ease', 'offset', 'fit', 'setBins', 'setSmooth', 'setScale', 'setCutoff', 'show', 'hide', 'fft']
window.__completionNames = () => [...new Set([...Object.keys(hydra.generator.glslTransforms), ...HYDRA_GLOBALS])].join('\n')

// The definitions of the hydra functions for the signature help and the typed completions of src/ui.c
// (src/signature.h): "name<TAB>kind<TAB>input:type:default" per line, also for functions of setFunction.
// hydra-synth adds the texture input of combine and combineCoord functions as _c1 or _c0 (generator-factory.js).
const formatDefault = (value) => typeof value === 'number' && !Number.isNaN(value) ? String(value) : ''
const inputName = (name) => /^_c\d$/.test(name) ? 'texture' : name
// The functions outside the glsl list follow in the notation of js/api-signatures.js.
window.__functionSignatures = () => Object.values(hydra.generator.glslTransforms).map(({ name, type, inputs = [] }) =>
  [name, type, ...inputs.map((input) => `${inputName(input.name)}:${input.type}:${formatDefault(input.default)}`)].join('\t'))
  .concat(apiSignatureLines()).join('\n')

// The helpers of a setup file (such as setup-hydra.js) that the completions of a number argument offer, when they
// exist at run time.
const NUMBER_HELPERS = [
  ['cc[]', () => Array.isArray(window.cc)],
  ['hit()', () => typeof window.hit === 'function'],
  ['midi.beat', () => window.midi && 'beat' in window.midi],
  ['midi.bpm', () => window.midi && 'bpm' in window.midi],
  ['time', () => true],
  ['a.fft[]', () => window.a && window.a.fft !== undefined]
]
window.__numberHelpers = () => NUMBER_HELPERS.filter(([, exists]) => exists()).map(([name]) => name).join('\n')
