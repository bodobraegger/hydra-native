// WebGLRenderingContext (WebGL 1) for canvas.getContext('webgl'), drawn by src/webgl.c on the GLES 3 context of
// hydra-native. It covers the calls of p5.js 1.6.0 in WEBGL mode and the usual WebGL 1 API; the canvas is a
// source for hydra like a 2D canvas. Not covered: compressed textures and copyTexImage2D.
import { sourceHandle, surfaces } from './canvas.js'
import constants from './webgl-constants.js'

// The order of enum simple_call in src/webgl.c.
const CALLS = ['activeTexture', 'attachShader', 'bindBuffer', 'bindTexture', 'bindRenderbuffer', 'blendColor',
  'blendEquation', 'blendEquationSeparate', 'blendFunc', 'blendFuncSeparate', 'clear', 'clearColor', 'clearDepth',
  'clearStencil', 'colorMask', 'compileShader', 'cullFace', 'depthFunc', 'depthMask', 'depthRange', 'detachShader',
  'disable', 'disableVertexAttribArray', 'drawArrays', 'drawElements', 'enable', 'enableVertexAttribArray', 'finish',
  'flush', 'framebufferRenderbuffer', 'framebufferTexture2D', 'frontFace', 'generateMipmap', 'hint', 'lineWidth',
  'linkProgram', 'polygonOffset', 'renderbufferStorage', 'scissor', 'stencilFunc', 'stencilMask', 'stencilOp',
  'texParameterf', 'texParameteri', 'uniform1f', 'uniform2f', 'uniform3f', 'uniform4f', 'uniform1i', 'uniform2i',
  'uniform3i', 'uniform4i', 'useProgram', 'validateProgram', 'vertexAttrib1f', 'vertexAttrib2f', 'vertexAttrib3f',
  'vertexAttrib4f', 'vertexAttribPointer', 'viewport']
const DRAWING_CALLS = new Set(['clear', 'drawArrays', 'drawElements'])
const SUPPORTED_EXTENSIONS = ['OES_element_index_uint', 'OES_standard_derivatives', 'OES_texture_float',
  'OES_texture_float_linear']

class WebGLObject {
  constructor (id) { this.id = id }
}
class WebGLUniformLocation {
  constructor (location, program) {
    this.location = location
    this.program = program
  }
}
class WebGLActiveInfo {
  constructor ([name, size, type]) { Object.assign(this, { name, size, type }) }
}

// The objects of createBuffer() and deleteBuffer(), and so on; createShader takes the shader type.
const OBJECT_KINDS = ['Buffer', 'Texture', 'Framebuffer', 'Renderbuffer', 'Program', 'Shader']
// The order of the `kind` argument of glUniformv in src/webgl.c; the data is the last argument.
const UNIFORM_VECTORS = ['1fv', '2fv', '3fv', '4fv', '1iv', '2iv', '3iv', '4iv', 'Matrix2fv', 'Matrix3fv', 'Matrix4fv']

const id = (object) => (object ? object.id : 0)
const location = (uniform) => (uniform ? uniform.location : -1)

export class WebGLRenderingContext {
  constructor (canvas, attributes = {}) {
    this.canvas = canvas
    this.attributes = { alpha: true, depth: true, stencil: true, antialias: false, premultipliedAlpha: true,
      preserveDrawingBuffer: true, powerPreference: 'default', failIfMajorPerformanceCaveat: false, ...attributes }
    this.context = native.glContextCreate(canvas.width, canvas.height)
    const gl = this
    CALLS.forEach((name, index) => {
      gl[name] = (...args) => {
        const numbers = args.map((value) => (value instanceof WebGLObject ? value.id
          : value instanceof WebGLUniformLocation ? value.location
          : value === true ? 1 : value === false ? 0 : value == null ? 0 : value))
        native.glCall(gl.context, index, ...numbers)
        if (DRAWING_CALLS.has(name)) canvas.changed()
      }
    })
  }

  get drawingBufferWidth () { return this.canvas.width }
  get drawingBufferHeight () { return this.canvas.height }
  getContextAttributes () { return { ...this.attributes } }
  isContextLost () { return false }
  getSupportedExtensions () { return SUPPORTED_EXTENSIONS.slice() }
  getExtension (name) { return SUPPORTED_EXTENSIONS.includes(name) ? {} : null }
  getError () { return native.glError(this.context) }
  getParameter (name) {
    const value = native.glParameter(this.context, name)
    if (name === this.VIEWPORT || name === this.SCISSOR_BOX || name === this.MAX_VIEWPORT_DIMS) return Int32Array.from(value)
    if (Array.isArray(value)) return Float32Array.from(value)
    return value
  }

  isEnabled (capability) { return native.glIsEnabled(this.context, capability) }

  bindFramebuffer (target, framebuffer) { native.glBindFramebuffer(this.context, target, id(framebuffer)) }

  bufferData (target, dataOrSize, usage) { native.glBufferData(this.context, target, dataOrSize, usage) }
  bufferSubData (target, offset, data) { native.glBufferSubData(this.context, target, offset, data) }

  shaderSource (shader, source) { native.glShaderSource(this.context, shader.id, source) }
  getShaderParameter (shader, name) {
    const value = native.glShaderParameter(this.context, shader.id, name)
    return name === this.COMPILE_STATUS || name === this.DELETE_STATUS ? value !== 0 : value
  }

  getProgramParameter (program, name) {
    const value = native.glProgramParameter(this.context, program.id, name)
    return [this.LINK_STATUS, this.DELETE_STATUS, this.VALIDATE_STATUS].includes(name) ? value !== 0 : value
  }

  getShaderInfoLog (shader) { return native.glShaderInfoLog(this.context, shader.id) }
  getProgramInfoLog (program) { return native.glProgramInfoLog(this.context, program.id) }
  bindAttribLocation (program, index, name) { native.glBindAttribLocation(this.context, program.id, index, name) }
  getAttribLocation (program, name) { return native.glAttribLocation(this.context, program.id, name) }
  getUniformLocation (program, name) {
    const value = native.glUniformLocation(this.context, program.id, name)
    return value < 0 ? null : new WebGLUniformLocation(value, program)
  }

  getActiveUniform (program, index) { return new WebGLActiveInfo(native.glActiveInfo(this.context, program.id, index, true)) }
  getActiveAttrib (program, index) { return new WebGLActiveInfo(native.glActiveInfo(this.context, program.id, index, false)) }

  pixelStorei (name, value) { native.glPixelStore(this.context, name, Number(value)) }

  // texImage2D(target, level, internalFormat, width, height, border, format, type, pixels) or
  // texImage2D(target, level, internalFormat, format, type, source) with a canvas, image, ImageData or video source.
  texImage2D (target, level, internalFormat, ...rest) {
    if (rest.length >= 6) {
      const [width, height, , format, type, pixels] = rest
      return native.glTexImage2D(this.context, target, level, internalFormat, width, height, format, type, pixels ?? null)
    }
    const [format, type, source] = rest
    if (source && source.data && source.width !== undefined && !source.nativeId) { // ImageData
      return native.glTexImage2D(this.context, target, level, internalFormat, source.width, source.height, format, type,
        source.data)
    }
    const handle = sourceHandle(source)
    if (handle < 0) throw new TypeError('texImage2D: hydra-native takes canvases, images, ImageData and video sources')
    native.glTexImageSource(this.context, target, level, internalFormat, format, type, handle)
  }

  // texSubImage2D(target, level, x, y, width, height, format, type, pixels) or
  // texSubImage2D(target, level, x, y, format, type, source), with the sources of texImage2D.
  texSubImage2D (target, level, x, y, ...rest) {
    if (rest.length >= 5) {
      const [width, height, format, type, pixels] = rest
      return native.glTexSubImage2D(this.context, target, level, x, y, width, height, format, type, pixels)
    }
    const [format, type, source] = rest
    if (source && source.data && source.width !== undefined && !source.nativeId) { // ImageData
      return native.glTexSubImage2D(this.context, target, level, x, y, source.width, source.height, format, type, source.data)
    }
    const handle = sourceHandle(source)
    if (handle < 0) throw new TypeError('texSubImage2D: hydra-native takes canvases, images, ImageData and video sources')
    native.glTexSubImageSource(this.context, target, level, x, y, format, type, handle)
  }

  readPixels (x, y, width, height, format, type, pixels) {
    native.glReadPixels(this.context, x, y, width, height, format, type, pixels)
  }

  resize (width, height) { native.glContextResize(this.context, width, height) }
  copyTo (texture) { native.glContextCopyTo(this.context, texture.id) }

  // A 2D canvas with a copy of the drawing buffer, for drawImage, patterns and texImage2D of this canvas.
  // ponytail: reads the pixels back on every use; cache per frame if a sketch draws the canvas many times.
  snapshot () {
    if (this.snapshotHandle === undefined) {
      this.snapshotHandle = native.canvasCreate(1, 1)
      surfaces.register(this, this.snapshotHandle)
    }
    native.glContextReadInto(this.context, this.snapshotHandle, this.attributes.premultipliedAlpha)
    return this.snapshotHandle
  }
}
for (const kind of OBJECT_KINDS) {
  WebGLRenderingContext.prototype[`create${kind}`] = function (...args) {
    return new WebGLObject(native[`glCreate${kind}`](this.context, ...args))
  }
  WebGLRenderingContext.prototype[`delete${kind}`] = function (object) {
    if (object) native[`glDelete${kind}`](this.context, object.id)
  }
}
UNIFORM_VECTORS.forEach((name, kind) => {
  WebGLRenderingContext.prototype[`uniform${name}`] = function (uniform, ...rest) {
    native.glUniformv(this.context, kind, location(uniform), Array.from(rest[rest.length - 1]))
  }
})
Object.assign(WebGLRenderingContext.prototype, constants)
Object.assign(WebGLRenderingContext, constants)

