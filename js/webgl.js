// WebGLRenderingContext (WebGL 1) for canvas.getContext('webgl'), drawn by src/webgl.c on the GLES 3 context of
// hydra-native. It covers the calls of p5.js 1.6.0 in WEBGL mode and the usual WebGL 1 API; the canvas is a
// source for hydra like a 2D canvas. Not covered: texSubImage2D, compressed textures and copyTexImage2D.
import { sourceHandle } from './canvas.js'
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
export class WebGLBuffer extends WebGLObject {}
export class WebGLTexture extends WebGLObject {}
export class WebGLProgram extends WebGLObject {}
export class WebGLShader extends WebGLObject {}
export class WebGLFramebuffer extends WebGLObject {}
export class WebGLRenderbuffer extends WebGLObject {}
export class WebGLUniformLocation {
  constructor (location, program) {
    this.location = location
    this.program = program
  }
}
export class WebGLActiveInfo {
  constructor ([name, size, type]) { Object.assign(this, { name, size, type }) }
}

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

  createBuffer () { return new WebGLBuffer(native.glCreateBuffer(this.context)) }
  createTexture () { return new WebGLTexture(native.glCreateTexture(this.context)) }
  createFramebuffer () { return new WebGLFramebuffer(native.glCreateFramebuffer(this.context)) }
  createRenderbuffer () { return new WebGLRenderbuffer(native.glCreateRenderbuffer(this.context)) }
  createProgram () { return new WebGLProgram(native.glCreateProgram(this.context)) }
  createShader (type) { return new WebGLShader(native.glCreateShader(this.context, type)) }
  deleteBuffer (object) { if (object) native.glDeleteBuffer(this.context, object.id) }
  deleteTexture (object) { if (object) native.glDeleteTexture(this.context, object.id) }
  deleteFramebuffer (object) { if (object) native.glDeleteFramebuffer(this.context, object.id) }
  deleteRenderbuffer (object) { if (object) native.glDeleteRenderbuffer(this.context, object.id) }
  deleteProgram (object) { if (object) native.glDeleteProgram(this.context, object.id) }
  deleteShader (object) { if (object) native.glDeleteShader(this.context, object.id) }
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

  uniform1fv (uniform, data) { native.glUniformv(this.context, 0, location(uniform), Array.from(data)) }
  uniform2fv (uniform, data) { native.glUniformv(this.context, 1, location(uniform), Array.from(data)) }
  uniform3fv (uniform, data) { native.glUniformv(this.context, 2, location(uniform), Array.from(data)) }
  uniform4fv (uniform, data) { native.glUniformv(this.context, 3, location(uniform), Array.from(data)) }
  uniform1iv (uniform, data) { native.glUniformv(this.context, 4, location(uniform), Array.from(data)) }
  uniform2iv (uniform, data) { native.glUniformv(this.context, 5, location(uniform), Array.from(data)) }
  uniform3iv (uniform, data) { native.glUniformv(this.context, 6, location(uniform), Array.from(data)) }
  uniform4iv (uniform, data) { native.glUniformv(this.context, 7, location(uniform), Array.from(data)) }
  uniformMatrix2fv (uniform, transpose, data) { native.glUniformv(this.context, 8, location(uniform), Array.from(data)) }
  uniformMatrix3fv (uniform, transpose, data) { native.glUniformv(this.context, 9, location(uniform), Array.from(data)) }
  uniformMatrix4fv (uniform, transpose, data) { native.glUniformv(this.context, 10, location(uniform), Array.from(data)) }

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
      snapshots.register(this, this.snapshotHandle)
    }
    native.glContextReadInto(this.context, this.snapshotHandle, this.attributes.premultipliedAlpha)
    return this.snapshotHandle
  }
}
const snapshots = new FinalizationRegistry((handle) => native.canvasDestroy(handle))
Object.assign(WebGLRenderingContext.prototype, constants)

