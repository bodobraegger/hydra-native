// The subset of regl that hydra-synth (output.js, hydra-source.js, hydra-synth.js) and the
// hydra-extensions use, implemented with the GLES calls of the `native` object.
// Every draw is one full-screen triangle with the attribute `position` at location 0 (see src/main.c).

const WRAP_CODES = { clamp: 0, repeat: 1, mirror: 2 }
// hydra-synth replaces the texture of a source at each init call and never destroys the old one, so the GL
// texture goes when its object is garbage.
const unusedTextures = new FinalizationRegistry((id) => native.deleteTexture(id))

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
    unusedTextures.register(this, this.id, this.registration = {})
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

  destroy () {
    unusedTextures.unregister(this.registration)
    native.deleteTexture(this.id)
  }
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

export default function createRegl () {
  const programs = new Map()
  const context = {}

  // The driver compiles a program in the background (src/programs.c); a failed program leaves the cache, so that the
  // next eval of the same code compiles it again and reports the error again.
  function program (vert, frag) {
    const key = vert + '\0' + frag
    if (!programs.has(key)) programs.set(key, { key, id: native.createProgram(vert, frag), linked: false, error: null })
    return programs.get(key)
  }

  // true when the program is linked, false while the driver compiles it (only if `wait` is false); throws the
  // shader error.
  function linked (entry, wait) {
    if (entry.error) throw entry.error
    if (entry.linked) return true
    try {
      entry.linked = native.programStatus(entry.id, wait)
    } catch (error) {
      entry.error = error
      programs.delete(entry.key)
      throw error
    }
    return entry.linked
  }

  // A command draws with its program as soon as it is called; command.ready() lets a caller keep an older
  // command until the driver is done (js/main.js does so for the outputs). When nothing compiles in the
  // background, the command waits at creation, so that GlslSource.out() catches the shader error.
  function regl ({ vert, frag, uniforms = {}, framebuffer }) {
    const entry = program(vert, frag)
    if (native.shaderCompile === 'blocking') linked(entry, true)
    let bindings = null

    const command = (props = {}) => {
      if (!bindings) {
        linked(entry, true)
        bindings = Object.entries(uniforms)
          .map(([name, value]) => [native.uniformLocation(entry.id, name), value])
          .filter(([location]) => location >= 0)
      }
      // regl resolves the framebuffer before the uniforms; hydra's ping-pong buffers depend on this order.
      const target = typeof framebuffer === 'function' ? framebuffer(context, props) : framebuffer
      // The window shows the canvas stretched, as the browser does with CSS, so the screen viewport is the
      // window size even after setResolution().
      const [width, height] = target ? [target.width, target.height] : native.drawableSize()
      context.viewportWidth = context.drawingBufferWidth = width
      context.viewportHeight = context.drawingBufferHeight = height
      native.bindFramebuffer(target ? target.id : 0, width, height)
      native.useProgram(entry.id)
      const textureUnit = { next: 0 }
      for (const [location, binding] of bindings) {
        let value = binding
        if (value instanceof Prop) value = props[value.name]
        else if (typeof value === 'function') value = value(context, props)
        setUniform(location, value, textureUnit)
      }
      native.drawTriangle()
    }
    command.ready = () => linked(entry, false)
    return command
  }

  regl.prop = (name) => new Prop(name)
  regl.texture = (options) => new Texture(options)
  regl.framebuffer = (options) => new Framebuffer(options)
  regl.buffer = (data) => data
  regl.clear = ({ color = [0, 0, 0, 0] } = {}) => {
    native.bindFramebuffer(0, ...native.drawableSize())
    native.clear(...color)
  }
  regl._refresh = () => {}
  regl.destroy = () => {}
  return regl
}
