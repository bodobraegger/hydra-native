// The subset of regl that hydra-synth (output.js, hydra-source.js, hydra-synth.js) and the
// hydra-extensions use, implemented with the GLES calls of the `native` object.
// Every draw is one full-screen triangle with the attribute `position` at location 0 (see src/main.c).

const WRAP_CODES = { clamp: 0, repeat: 1, mirror: 2 }

class Prop {
  constructor (name) { this.name = name }
}

class Texture {
  constructor (options = {}) {
    const { shape, data, mag = 'nearest', min = 'nearest', wrap = 'clamp' } = options
    let { width = 1, height = 1 } = options
    if (shape) [width, height] = shape
    if (data && data.width) ({ width, height } = data)
    const [wrapS, wrapT] = Array.isArray(wrap) ? wrap : [options.wrapS || wrap, options.wrapT || wrap]
    this.width = width
    this.height = height
    this.id = native.createTexture(width, height, mag === 'linear', min === 'linear',
      WRAP_CODES[wrapS] ?? 0, WRAP_CODES[wrapT] ?? 0)
    if (data) this.subimage(data)
  }

  get textureId () { return this.id }

  resize (width, height) {
    this.width = width
    this.height = height
    native.resizeTexture(this.id, width, height)
    return this
  }

  // `source` is a native video source from sources.js; DOM media elements do not exist here.
  subimage (source) {
    if (source && source.upload) source.upload(this)
    return this
  }

  destroy () { native.deleteTexture(this.id) }
}

class Framebuffer {
  constructor (options = {}) {
    const color = options.color instanceof Texture ? options.color : new Texture(options)
    this.color = [color]
    this.id = native.createFramebuffer(color.id)
  }

  get textureId () { return this.color[0].id }
  get width () { return this.color[0].width }
  get height () { return this.color[0].height }

  resize (width, height) {
    this.color[0].resize(width, height)
    return this
  }

  destroy () { this.color[0].destroy() }
}

function setUniform (location, value, textureUnit) {
  if (typeof value === 'number') native.uniform(location, value)
  else if (typeof value === 'boolean') native.uniform(location, value ? 1 : 0)
  else if (value && value.textureId !== undefined) native.uniformTexture(location, textureUnit.next++, value.textureId)
  else if (value && value.length) native.uniform(location, ...value)
}

// The window shows the canvas stretched, as the browser does with CSS, so the screen viewport is the window size
// even after setResolution().
function screenSize () {
  return native.drawableSize()
}

export default function createRegl () {
  const programs = new Map()
  const context = {}

  function program (vert, frag) {
    const key = vert + '\0' + frag
    if (!programs.has(key)) programs.set(key, native.createProgram(vert, frag))
    return programs.get(key)
  }

  // Compiles at creation, unlike regl, so that GlslSource.out() catches shader errors and keeps the old pass.
  function regl ({ vert, frag, uniforms = {}, framebuffer }) {
    const id = program(vert, frag)
    const bindings = Object.entries(uniforms)
      .map(([name, value]) => [native.uniformLocation(id, name), value])
      .filter(([location]) => location >= 0)

    return (props = {}) => {
      // regl resolves the framebuffer before the uniforms; hydra's ping-pong buffers depend on this order.
      const target = typeof framebuffer === 'function' ? framebuffer(context, props) : framebuffer
      const [width, height] = target ? [target.width, target.height] : screenSize()
      context.viewportWidth = context.drawingBufferWidth = width
      context.viewportHeight = context.drawingBufferHeight = height
      native.bindFramebuffer(target ? target.id : 0, width, height)
      native.useProgram(id)
      const textureUnit = { next: 0 }
      for (const [location, binding] of bindings) {
        let value = binding
        if (value instanceof Prop) value = props[value.name]
        else if (typeof value === 'function') value = value(context, props)
        setUniform(location, value, textureUnit)
      }
      native.drawTriangle()
    }
  }

  regl.prop = (name) => new Prop(name)
  regl.texture = (options) => new Texture(options)
  regl.framebuffer = (options) => new Framebuffer(options)
  regl.buffer = (data) => data
  regl.clear = ({ color = [0, 0, 0, 0] } = {}) => {
    native.bindFramebuffer(0, ...screenSize())
    native.clear(...color)
  }
  regl._refresh = () => {}
  regl.destroy = () => {}
  return regl
}
