// Runs with `make check`: renders a chain into o0 and a feedback chain into o1,
// reads pixels back and exits with status 0 when both outputs contain color and the 2D canvas checks pass.
const CHECK_DELAY_MS = 500

osc(10, 0.1, 1.2).rotate(0.3).modulate(noise(3), 0.2).out(o0)
src(o1).scale(1.01).blend(shape(4, 0.5).color(1, 0.5, 0), 0.5).out(o1)
render(o0)

// 2D canvas: filters, a shadow within the clip, drawImage of a WebGL canvas. The values are those of Chrome.
function canvasChecks () {
  const canvas = document.createElement('canvas')
  canvas.width = 40
  canvas.height = 20
  const context = canvas.getContext('2d')
  const pixel = (x, y) => Array.from(context.getImageData(x, y, 1, 1).data)
  const near = (actual, expected) => actual.every((value, i) => Math.abs(value - expected[i]) <= 2)
  const failures = []
  const expect = (name, actual, expected) => { if (!near(actual, expected)) failures.push(`${name}: ${actual} instead of ${expected}`) }

  context.filter = 'grayscale(1)'
  context.fillStyle = 'red'
  context.fillRect(0, 0, 10, 10)
  expect('grayscale', pixel(5, 5), [54, 54, 54, 255])
  context.filter = 'not a filter' // ignored
  context.filter = 'drop-shadow(10px 0 0 blue) invert(0)'
  context.fillStyle = 'white'
  context.fillRect(20, 0, 5, 5)
  expect('drop-shadow', pixel(32, 2), [0, 0, 255, 255])
  context.filter = 'none'

  context.clearRect(0, 0, 40, 20)
  context.save()
  context.beginPath()
  context.rect(0, 0, 20, 20)
  context.clip()
  context.shadowColor = 'lime'
  context.shadowOffsetX = 15
  context.fillStyle = 'black'
  context.fillRect(0, 0, 10, 10)
  context.restore()
  expect('shadow inside the clip', pixel(16, 5), [0, 255, 0, 255])
  expect('shadow outside the clip', pixel(22, 5), [0, 0, 0, 0])

  context.fillStyle = 'white'
  context.fillRect(0, 0, 40, 20)
  context.save()
  context.beginPath()
  context.rect(0, 0, 20, 20)
  context.clip()
  context.globalCompositeOperation = 'multiply'
  context.fillStyle = 'red'
  context.fillRect(0, 0, 40, 20)
  context.restore()
  expect('multiply inside the clip', pixel(5, 5), [255, 0, 0, 255])
  expect('multiply outside the clip', pixel(30, 5), [255, 255, 255, 255])

  const webglCanvas = document.createElement('canvas')
  webglCanvas.width = 4
  webglCanvas.height = 4
  const gl = webglCanvas.getContext('webgl')
  gl.clearColor(0, 1, 0, 1)
  gl.clear(gl.COLOR_BUFFER_BIT)
  gl.enable(gl.SCISSOR_TEST)
  gl.scissor(0, 0, 4, 2) // the lower half in WebGL coordinates
  gl.clearColor(1, 0, 0, 1)
  gl.clear(gl.COLOR_BUFFER_BIT)
  context.clearRect(0, 0, 40, 20)
  context.drawImage(webglCanvas, 0, 0)
  expect('WebGL canvas, top', pixel(1, 0), [0, 255, 0, 255])
  expect('WebGL canvas, bottom', pixel(1, 3), [255, 0, 0, 255])

  const texture = gl.createTexture()
  gl.bindTexture(gl.TEXTURE_2D, texture)
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 2, 2, 0, gl.RGBA, gl.UNSIGNED_BYTE, null)
  gl.texSubImage2D(gl.TEXTURE_2D, 0, 1, 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([255, 0, 255, 255]))
  const framebuffer = gl.createFramebuffer()
  gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer)
  gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, texture, 0)
  const texel = new Uint8Array(4)
  gl.readPixels(1, 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, texel)
  gl.bindFramebuffer(gl.FRAMEBUFFER, null)
  expect('texSubImage2D', Array.from(texel), [255, 0, 255, 255])
  return failures
}

// fetch and XMLHttpRequest of a local file, as text and as bytes (p5.js loadFont reads fonts so).
function networkChecks () {
  const path = native.directory + '/p5.min.js'
  const text = fetch(path).then((response) => response.text())
  const bytes = new Promise((resolve) => {
    const request = new XMLHttpRequest()
    request.open('GET', path)
    request.responseType = 'arraybuffer'
    request.onload = () => resolve(request.response)
    request.onerror = () => resolve(null)
    request.send()
  })
  return Promise.all([text, bytes]).then(([code, buffer]) => {
    const failures = []
    if (!code.includes('p5')) failures.push('fetch: no text')
    if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < code.length) failures.push('XMLHttpRequest: no bytes')
    return failures
  }, (error) => [`fetch: ${error.message}`])
}

const canvasFailures = canvasChecks()
networkChecks().then((failures) => canvasFailures.push(...failures))

setTimeout(() => {
  const x = Math.floor(width / 2), y = Math.floor(height / 2)
  const pixels = [o0, o1].map((output) => native.readPixel(output.getCurrent().id, x, y))
  const ok = pixels.every(([r, g, b]) => r + g + b > 0) && !canvasFailures.length
  console.log(JSON.stringify({ check: ok ? 'ok' : 'failed', size: [width, height], o0: pixels[0], o1: pixels[1], canvasFailures }))
  native.exit(ok ? 0 : 1)
}, CHECK_DELAY_MS)
