// text(), strokeText(), fillStrokeText() and strokeFillText() with the interface of the hydra-text extension
// (hydra-extensions/hydra-text.js, by Ritchse): the same arguments, the same `hydraText` settings and the same
// layout. The extension draws on a 2D canvas, which hydra-native does not have; here src/text.c draws the text
// into the texture of a source. Differences: lineJoin has no effect, and a line wider than the texture is not
// squeezed.
import { parseColor } from './color.js'

const DEFAULTS = {
  font: 'sans-serif',
  fontStyle: 'normal',
  fontSize: 'auto',
  textAlign: 'center',
  fillStyle: 'white',
  strokeStyle: 'white',
  lineWidth: '2%',
  lineJoin: 'miter',
  canvasResize: 2,
  interpolation: 'linear'
}
const PADDING_PART = 1 / 20 // of the output width, shared by both sides
const LINE_HEIGHT = 1.4 // font sizes per line
const ALIGNMENTS = { left: 0, start: 0, center: 1, right: 2, end: 2 }
const FILL = 1
const STROKE = 2
const FILL_AFTER = 4
const WHITE = [1, 1, 1, 1]

const isPercentage = (value) => String(value).endsWith('%')
const percentage = (value) => parseFloat(value) / 100
const packColor = (value) => (parseColor(value) || WHITE).reduce((packed, channel) => packed * 256 + Math.round(channel * 255), 0)

export function installText (hydra) {
  const Source = hydra.s[0].constructor
  const textures = new FinalizationRegistry((id) => native.deleteTexture(id))
  window.hydraText = { ...DEFAULTS }

  // The source masked to its own aspect ratio, so that the text keeps its proportions in the output.
  function srcRelMask (source) {
    if (!Object.hasOwn(source, 'src')) return src(source)
    const sourceRatio = () => source.src?.width ? source.src.width / source.src.height : 0
    const outputRatio = () => hydra.canvas.width / hydra.canvas.height
    return src(source).mask(shape(4, 1, 0)).scale(1,
      () => outputRatio() > sourceRatio() ? sourceRatio() / outputRatio() : 1,
      () => 1 / outputRatio() > 1 / sourceRatio() ? outputRatio() / sourceRatio() : 1)
  }

  function render (text, config, passes) {
    if (typeof config === 'string') config = { font: config }
    config = { ...DEFAULTS, ...window.hydraText, ...config }
    const lines = String(text).split('\n')
    const longestLine = lines.reduce((a, b) => a.length > b.length ? a : b)
    const bold = /bold/.test(config.fontStyle)
    const italic = /italic|oblique/.test(config.fontStyle)
    const scale = Number(config.canvasResize) || 1
    const padding = hydra.width * PADDING_PART
    let fontSize = (hydra.width - padding) / (native.textWidth(config.font, bold, italic, longestLine) || 1)
    const height = Math.ceil(fontSize * LINE_HEIGHT * lines.length * scale)
    if (isPercentage(config.fontSize)) fontSize *= percentage(config.fontSize)
    else if (config.fontSize !== 'auto') fontSize = parseFloat(String(config.fontSize).replace(/[^0-9.,]+/, ''))
    const lineWidth = isPercentage(config.lineWidth) ? fontSize * percentage(config.lineWidth) : parseFloat(config.lineWidth)
    const width = Math.ceil(hydra.width * scale)
    const align = ALIGNMENTS[config.textAlign] ?? ALIGNMENTS.center
    const x = [padding * scale / 2, width / 2, width - padding * scale / 2][align]

    const source = new Source({ regl: hydra.regl, pb: hydra.pb, width: hydra.width, height: hydra.height })
    source.src = { width, height }
    source.tex = hydra.regl.texture({ width, height, min: config.interpolation, mag: config.interpolation })
    textures.register(source, source.tex.id)
    native.textTexture(source.tex.id, lines.join('\n'), config.font, bold, italic, width, height, fontSize * scale, x, align,
      packColor(config.fillStyle), packColor(config.strokeStyle), lineWidth * scale, passes)
    return srcRelMask(source)
  }

  // A sketch can replace these with a value or a declaration. The assignment of a function is ignored, so that
  // the hydra-text extension, when a sketch loads it, does not replace them with its canvas versions.
  const builtIn = (name, value) => Object.defineProperty(window, name, {
    configurable: true,
    get: () => value,
    set: (other) => { if (typeof other !== 'function') Object.defineProperty(window, name, { value: other, writable: true }) }
  })
  builtIn('srcRelMask', srcRelMask)
  builtIn('text', (text, config) => render(text, config, FILL))
  builtIn('strokeText', (text, config) => render(text, config, STROKE))
  builtIn('fillStrokeText', (text, config) => render(text, config, FILL | STROKE))
  builtIn('strokeFillText', (text, config) => render(text, config, STROKE | FILL_AFTER))
}
