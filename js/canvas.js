// A <canvas> element with a CanvasRenderingContext2D, drawn by src/canvas2d.c (plutovg).
// It covers what hydra-text, hydra-canvas and s0.initCanvas() sketches use: rectangles, paths, arcs, text,
// transforms, clipping, globalAlpha, composite operations and drawImage of other canvases.
// Not covered: gradients, patterns, shadows, filters, line dashes, getImageData and putImageData;
// roundRect draws a plain rectangle.

const PATH_MOVE = 0
const PATH_LINE = 1
const PATH_QUAD = 2
const PATH_CUBIC = 3
const PATH_ARC = 4
const PATH_ELLIPSE = 5
const PATH_RECT = 6
const PATH_CLOSE = 7
const RECT_FILL = 0
const RECT_STROKE = 1
const RECT_CLEAR = 2
const DEFAULT_WIDTH = 300
const DEFAULT_HEIGHT = 150
const DEFAULT_FONT = '10px sans-serif'
const CSS_PIXELS_PER_POINT = 4 / 3
const CSS_PIXELS_PER_EM = 16

const LINE_CAPS = { butt: 0, round: 1, square: 2 }
const LINE_JOINS = { miter: 0, round: 1, bevel: 2 }
// Indexes of plutovg_operator_t; other CSS modes fall back to source-over.
const OPERATORS = {
  copy: 1,
  'source-over': 3,
  'destination-over': 4,
  'source-in': 5,
  'destination-in': 6,
  'source-out': 7,
  'destination-out': 8,
  'source-atop': 9,
  'destination-atop': 10,
  xor: 11
}

const NAMED_COLORS = {
  aliceblue: 'f0f8ff', antiquewhite: 'faebd7', aqua: '00ffff', aquamarine: '7fffd4', azure: 'f0ffff', beige: 'f5f5dc',
  bisque: 'ffe4c4', black: '000000', blanchedalmond: 'ffebcd', blue: '0000ff', blueviolet: '8a2be2', brown: 'a52a2a',
  burlywood: 'deb887', cadetblue: '5f9ea0', chartreuse: '7fff00', chocolate: 'd2691e', coral: 'ff7f50',
  cornflowerblue: '6495ed', cornsilk: 'fff8dc', crimson: 'dc143c', cyan: '00ffff', darkblue: '00008b', darkcyan: '008b8b',
  darkgoldenrod: 'b8860b', darkgray: 'a9a9a9', darkgreen: '006400', darkgrey: 'a9a9a9', darkkhaki: 'bdb76b',
  darkmagenta: '8b008b', darkolivegreen: '556b2f', darkorange: 'ff8c00', darkorchid: '9932cc', darkred: '8b0000',
  darksalmon: 'e9967a', darkseagreen: '8fbc8f', darkslateblue: '483d8b', darkslategray: '2f4f4f', darkslategrey: '2f4f4f',
  darkturquoise: '00ced1', darkviolet: '9400d3', deeppink: 'ff1493', deepskyblue: '00bfff', dimgray: '696969',
  dimgrey: '696969', dodgerblue: '1e90ff', firebrick: 'b22222', floralwhite: 'fffaf0', forestgreen: '228b22',
  fuchsia: 'ff00ff', gainsboro: 'dcdcdc', ghostwhite: 'f8f8ff', gold: 'ffd700', goldenrod: 'daa520', gray: '808080',
  green: '008000', greenyellow: 'adff2f', grey: '808080', honeydew: 'f0fff0', hotpink: 'ff69b4', indianred: 'cd5c5c',
  indigo: '4b0082', ivory: 'fffff0', khaki: 'f0e68c', lavender: 'e6e6fa', lavenderblush: 'fff0f5', lawngreen: '7cfc00',
  lemonchiffon: 'fffacd', lightblue: 'add8e6', lightcoral: 'f08080', lightcyan: 'e0ffff', lightgoldenrodyellow: 'fafad2',
  lightgray: 'd3d3d3', lightgreen: '90ee90', lightgrey: 'd3d3d3', lightpink: 'ffb6c1', lightsalmon: 'ffa07a',
  lightseagreen: '20b2aa', lightskyblue: '87cefa', lightslategray: '778899', lightslategrey: '778899',
  lightsteelblue: 'b0c4de', lightyellow: 'ffffe0', lime: '00ff00', limegreen: '32cd32', linen: 'faf0e6',
  magenta: 'ff00ff', maroon: '800000', mediumaquamarine: '66cdaa', mediumblue: '0000cd', mediumorchid: 'ba55d3',
  mediumpurple: '9370db', mediumseagreen: '3cb371', mediumslateblue: '7b68ee', mediumspringgreen: '00fa9a',
  mediumturquoise: '48d1cc', mediumvioletred: 'c71585', midnightblue: '191970', mintcream: 'f5fffa', mistyrose: 'ffe4e1',
  moccasin: 'ffe4b5', navajowhite: 'ffdead', navy: '000080', oldlace: 'fdf5e6', olive: '808000', olivedrab: '6b8e23',
  orange: 'ffa500', orangered: 'ff4500', orchid: 'da70d6', palegoldenrod: 'eee8aa', palegreen: '98fb98',
  paleturquoise: 'afeeee', palevioletred: 'db7093', papayawhip: 'ffefd5', peachpuff: 'ffdab9', peru: 'cd853f',
  pink: 'ffc0cb', plum: 'dda0dd', powderblue: 'b0e0e6', purple: '800080', rebeccapurple: '663399', red: 'ff0000',
  rosybrown: 'bc8f8f', royalblue: '4169e1', saddlebrown: '8b4513', salmon: 'fa8072', sandybrown: 'f4a460',
  seagreen: '2e8b57', seashell: 'fff5ee', sienna: 'a0522d', silver: 'c0c0c0', skyblue: '87ceeb', slateblue: '6a5acd',
  slategray: '708090', slategrey: '708090', snow: 'fffafa', springgreen: '00ff7f', steelblue: '4682b4', tan: 'd2b48c',
  teal: '008080', thistle: 'd8bfd8', tomato: 'ff6347', turquoise: '40e0d0', violet: 'ee82ee', wheat: 'f5deb3',
  white: 'ffffff', whitesmoke: 'f5f5f5', yellow: 'ffff00', yellowgreen: '9acd32'
}

function hexColor (hex) {
  if (hex.length <= 4) hex = [...hex].map((digit) => digit + digit).join('')
  const channels = [0, 2, 4, 6].map((offset) => parseInt(hex.slice(offset, offset + 2) || 'ff', 16) / 255)
  return channels
}

function hslToRgb (hue, saturation, lightness) {
  const k = (n) => (n + hue / 30) % 12
  const a = saturation * Math.min(lightness, 1 - lightness)
  return [0, 8, 4].map((n) => lightness - a * Math.max(-1, Math.min(k(n) - 3, 9 - k(n), 1)))
}

// Returns [r, g, b, a] in 0..1, or null for a value that is no CSS color.
export function parseColor (value) {
  const text = String(value).trim().toLowerCase()
  if (text === 'transparent') return [0, 0, 0, 0]
  if (NAMED_COLORS[text]) return hexColor(NAMED_COLORS[text])
  if (/^#([0-9a-f]{3,4}|[0-9a-f]{6}|[0-9a-f]{8})$/.test(text)) return hexColor(text.slice(1))
  const match = /^(rgba?|hsla?)\(([^)]*)\)$/.exec(text)
  if (!match) return null
  const parts = match[2].split(/[\s,/]+/).filter(Boolean)
  const channel = (part, scale) => part.endsWith('%') ? parseFloat(part) / 100 : parseFloat(part) / scale
  const alpha = parts.length > 3 ? channel(parts[3], 1) : 1
  if (match[1].startsWith('rgb')) return [...parts.slice(0, 3).map((part) => channel(part, 255)), alpha]
  return [...hslToRgb(parseFloat(parts[0]), channel(parts[1], 100), channel(parts[2], 100)), alpha]
}

// Parses the CSS font shorthand: [style] [variant] [weight] size[/line-height] family[, family ...].
function parseFont (value) {
  const match = /^\s*((?:[\w-]+\s+)*?)([\d.]+)(px|pt|em|rem|%)(?:\/\S+)?\s+(.+)$/.exec(String(value))
  if (!match) return null
  const modifiers = match[1].split(/\s+/)
  const unit = { px: 1, pt: CSS_PIXELS_PER_POINT, em: CSS_PIXELS_PER_EM, rem: CSS_PIXELS_PER_EM, '%': CSS_PIXELS_PER_EM / 100 }
  const family = match[4].split(',')[0].trim().replace(/^["']|["']$/g, '')
  return {
    family,
    size: parseFloat(match[2]) * unit[match[3]],
    bold: modifiers.some((word) => word === 'bold' || word === 'bolder' || parseInt(word) >= 600),
    italic: modifiers.some((word) => word === 'italic' || word === 'oblique')
  }
}

function multiply ([a1, b1, c1, d1, e1, f1], [a2, b2, c2, d2, e2, f2]) {
  return [a1 * a2 + c1 * b2, b1 * a2 + d1 * b2, a1 * c2 + c1 * d2, b1 * c2 + d1 * d2,
    a1 * e2 + c1 * f2 + e1, b1 * e2 + d1 * f2 + f1]
}

export class Path2D {
  constructor (path) {
    this.ops = path instanceof Path2D ? path.ops.slice() : []
  }

  moveTo (x, y) { this.ops.push(PATH_MOVE, x, y) }
  lineTo (x, y) { this.ops.push(PATH_LINE, x, y) }
  quadraticCurveTo (cx, cy, x, y) { this.ops.push(PATH_QUAD, cx, cy, x, y) }
  bezierCurveTo (c1x, c1y, c2x, c2y, x, y) { this.ops.push(PATH_CUBIC, c1x, c1y, c2x, c2y, x, y) }
  arc (x, y, radius, start, end, anticlockwise = false) { this.ops.push(PATH_ARC, x, y, radius, start, end, anticlockwise ? 1 : 0) }
  ellipse (x, y, radiusX, radiusY, rotation, start, end, anticlockwise = false) {
    this.ops.push(PATH_ELLIPSE, x, y, radiusX, radiusY, rotation, start, end, anticlockwise ? 1 : 0)
  }

  rect (x, y, width, height) { this.ops.push(PATH_RECT, x, y, width, height) }
  roundRect (x, y, width, height) { this.rect(x, y, width, height) }
  closePath () { this.ops.push(PATH_CLOSE) }
}

const STATE_KEYS = ['fillStyle', 'strokeStyle', 'lineWidth', 'lineCap', 'lineJoin', 'miterLimit', 'globalAlpha',
  'globalCompositeOperation', 'font', 'textAlign', 'textBaseline', 'matrix']

class CanvasRenderingContext2D {
  constructor (canvas) {
    this.canvas = canvas
    this.path = new Path2D()
    this.stack = []
    this.reset()
  }

  get id () { return this.canvas.nativeId() }

  reset () {
    Object.assign(this, {
      fillStyle: '#000000',
      strokeStyle: '#000000',
      lineWidth: 1,
      lineCap: 'butt',
      lineJoin: 'miter',
      miterLimit: 10,
      globalAlpha: 1,
      globalCompositeOperation: 'source-over',
      textAlign: 'start',
      textBaseline: 'alphabetic',
      matrix: [1, 0, 0, 1, 0, 0]
    })
    this._font = DEFAULT_FONT
    this.fontApplied = null
  }

  get font () { return this._font }
  set font (value) { if (parseFont(value)) this._font = value }

  save () {
    this.stack.push(Object.fromEntries(STATE_KEYS.map((key) => [key, this[key]])))
    native.canvasSave(this.id)
  }

  restore () {
    const state = this.stack.pop()
    if (!state) return
    Object.assign(this, state)
    native.canvasRestore(this.id)
    native.canvasSetTransform(this.id, ...this.matrix)
  }

  setTransform (a = 1, b = 0, c = 0, d = 1, e = 0, f = 0) {
    if (typeof a === 'object') ({ a, b, c, d, e, f } = a)
    this.matrix = [a, b, c, d, e, f]
    native.canvasSetTransform(this.id, ...this.matrix)
  }

  getTransform () {
    const [a, b, c, d, e, f] = this.matrix
    return { a, b, c, d, e, f }
  }

  resetTransform () { this.setTransform() }
  transform (a, b, c, d, e, f) { this.setTransform(...multiply(this.matrix, [a, b, c, d, e, f])) }
  translate (x, y) { this.transform(1, 0, 0, 1, x, y) }
  scale (x, y) { this.transform(x, 0, 0, y, 0, 0) }
  rotate (angle) { this.transform(Math.cos(angle), Math.sin(angle), -Math.sin(angle), Math.cos(angle), 0, 0) }

  paint (style) {
    const [r, g, b, a] = parseColor(typeof style === 'string' ? style : '#000') || [0, 0, 0, 1]
    native.canvasPaint(this.id, r, g, b, a, this.lineWidth, LINE_CAPS[this.lineCap] ?? 0, LINE_JOINS[this.lineJoin] ?? 0,
      this.miterLimit, this.globalAlpha, OPERATORS[this.globalCompositeOperation] ?? OPERATORS['source-over'])
    this.canvas.changed()
  }

  beginPath () { this.path = new Path2D() }
  moveTo (...args) { this.path.moveTo(...args) }
  lineTo (...args) { this.path.lineTo(...args) }
  quadraticCurveTo (...args) { this.path.quadraticCurveTo(...args) }
  bezierCurveTo (...args) { this.path.bezierCurveTo(...args) }
  arc (...args) { this.path.arc(...args) }
  ellipse (...args) { this.path.ellipse(...args) }
  rect (...args) { this.path.rect(...args) }
  roundRect (...args) { this.path.roundRect(...args) }
  closePath () { this.path.closePath() }

  pathAndRule (pathOrRule, rule) {
    const path = pathOrRule instanceof Path2D ? pathOrRule : this.path
    const fillRule = typeof pathOrRule === 'string' ? pathOrRule : rule
    native.canvasPath(this.id, path.ops)
    return fillRule === 'evenodd'
  }

  fill (pathOrRule, rule) {
    const evenOdd = this.pathAndRule(pathOrRule, rule)
    this.paint(this.fillStyle)
    native.canvasFill(this.id, evenOdd)
  }

  stroke (path) {
    native.canvasPath(this.id, (path instanceof Path2D ? path : this.path).ops)
    this.paint(this.strokeStyle)
    native.canvasStroke(this.id)
  }

  clip (pathOrRule, rule) {
    native.canvasClip(this.id, this.pathAndRule(pathOrRule, rule))
  }

  fillRect (x, y, width, height) {
    this.paint(this.fillStyle)
    native.canvasRect(this.id, RECT_FILL, x, y, width, height)
  }

  strokeRect (x, y, width, height) {
    this.paint(this.strokeStyle)
    native.canvasRect(this.id, RECT_STROKE, x, y, width, height)
  }

  clearRect (x, y, width, height) {
    native.canvasRect(this.id, RECT_CLEAR, x, y, width, height)
    this.canvas.changed()
  }

  applyFont () {
    if (this.fontApplied === this._font) return
    const { family, bold, italic, size } = parseFont(this._font)
    native.canvasFont(this.id, family, bold, italic, size)
    this.fontApplied = this._font
  }

  measureText (text) {
    this.applyFont()
    const [width, ascent, descent, left, top, right, bottom] = native.canvasMeasure(this.id, String(text))
    return {
      width,
      actualBoundingBoxLeft: -left,
      actualBoundingBoxRight: right,
      actualBoundingBoxAscent: -top,
      actualBoundingBoxDescent: bottom,
      fontBoundingBoxAscent: ascent,
      fontBoundingBoxDescent: -descent
    }
  }

  drawText (text, x, y, maxWidth, stroke) {
    text = String(text)
    const metrics = this.measureText(text)
    const align = { center: 0.5, right: 1, end: 1 }[this.textAlign] || 0
    const baseline = {
      top: metrics.fontBoundingBoxAscent,
      hanging: metrics.fontBoundingBoxAscent,
      middle: (metrics.fontBoundingBoxAscent - metrics.fontBoundingBoxDescent) / 2,
      bottom: -metrics.fontBoundingBoxDescent,
      ideographic: -metrics.fontBoundingBoxDescent
    }[this.textBaseline] || 0
    const squeeze = maxWidth !== undefined && metrics.width > maxWidth ? maxWidth / metrics.width : 1
    this.paint(stroke ? this.strokeStyle : this.fillStyle)
    if (squeeze !== 1) {
      native.canvasSetTransform(this.id, ...multiply(this.matrix, [squeeze, 0, 0, 1, x * (1 - squeeze), 0]))
    }
    native.canvasText(this.id, text, x - metrics.width * align, y + baseline, stroke)
    if (squeeze !== 1) native.canvasSetTransform(this.id, ...this.matrix)
  }

  fillText (text, x, y, maxWidth) { this.drawText(text, x, y, maxWidth, false) }
  strokeText (text, x, y, maxWidth) { this.drawText(text, x, y, maxWidth, true) }

  drawImage (image, ...args) {
    if (!image || !image.nativeId) return console.warn('drawImage: only canvases can be drawn')
    const [sx, sy, sw, sh, dx, dy, dw, dh] = args.length === 8
      ? args
      : [0, 0, image.width, image.height, args[0], args[1], args[2] ?? image.width, args[3] ?? image.height]
    this.paint(this.fillStyle)
    native.canvasDrawCanvas(this.id, image.nativeId(), sx, sy, sw, sh, dx, dy, dw, dh)
  }

  setLineDash () {}
  getLineDash () { return [] }
  createLinearGradient () { return unsupportedGradient() }
  createRadialGradient () { return unsupportedGradient() }
  createPattern () { return null }
}

function unsupportedGradient () {
  console.warn('canvas gradients are not supported in hydra-native')
  return { addColorStop () {} }
}

// Frees the native surface when the canvas object is garbage.
const surfaces = new FinalizationRegistry((id) => native.canvasDestroy(id))

let nextVersion = 1

export class CanvasElement {
  constructor () {
    this.tagName = 'CANVAS'
    this.style = {}
    this.children = []
    this.parentNode = null
    this._width = DEFAULT_WIDTH
    this._height = DEFAULT_HEIGHT
    this.handle = -1
    this.version = 0
    this.uploaded = ''
  }

  nativeId () {
    if (this.handle < 0) {
      this.handle = native.canvasCreate(this._width, this._height)
      surfaces.register(this, this.handle)
    }
    return this.handle
  }

  changed () { this.version = nextVersion++ }

  get width () { return this._width }
  get height () { return this._height }
  set width (value) { this.resize(value, this._height) }
  set height (value) { this.resize(this._width, value) }
  get clientWidth () { return this._width }
  get clientHeight () { return this._height }

  // Setting the size clears the canvas and its context state, as in the browser.
  resize (width, height) {
    this._width = Math.max(1, Math.floor(width) || 0)
    this._height = Math.max(1, Math.floor(height) || 0)
    if (this.handle >= 0) native.canvasResize(this.handle, this._width, this._height)
    if (this.context) this.context.reset()
    this.changed()
  }

  getContext (type) {
    if (type !== '2d') return null
    if (!this.context) {
      this.nativeId()
      this.context = new CanvasRenderingContext2D(this)
    }
    return this.context
  }

  // Called by the regl shim (Texture.subimage) every frame; uploads only after a change.
  upload (texture) {
    const key = `${texture.id}:${texture.width}x${texture.height}:${this.version}`
    if (this.uploaded === key || this.handle < 0) return
    native.canvasUpload(this.handle, texture.id)
    texture.width = this._width
    texture.height = this._height
    this.uploaded = `${texture.id}:${texture.width}x${texture.height}:${this.version}`
  }

  appendChild (child) { this.children.push(child); return child }
  removeChild (child) { this.children = this.children.filter((item) => item !== child); return child }
  remove () { if (this.parentNode) this.parentNode.removeChild(this) }
  setAttribute (name, value) { this[name] = value }
  addEventListener () {}
  removeEventListener () {}
  toDataURL () { return '' }
}
