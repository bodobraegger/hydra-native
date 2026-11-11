// Runs with `make check`: renders a chain into o0 and a feedback chain into o1,
// reads pixels back and exits with status 0 when both outputs contain color and the other checks pass.
const CHECK_DELAY_MS = 500
const PENDING_FRAMES = 3
const PENDING_MAX_FRAMES = 1000

osc(10, 0.1, 1.2).rotate(0.3).modulate(noise(3), 0.2).out(o0)
src(o1).scale(1.01).blend(shape(4, 0.5).color(1, 0.5, 0), 0.5).out(o1)
render(o0)

const frames = (count) => new Promise((resolve) => {
  const step = () => (count-- > 0 ? requestAnimationFrame(step) : resolve())
  step()
})

// The text sources of js/text.js: filled text gives pixels, outlined text gives other pixels, and the hydra-text
// extension, when a sketch loads it, does not replace them.
async function textChecks () {
  const SAMPLES = 24
  const sample = async (chain) => {
    solid(0, 0, 0).add(chain).out(o2)
    for (let frame = 0; frame < PENDING_MAX_FRAMES && o2.pendingDraw; frame++) await frames(1)
    await frames(PENDING_FRAMES)
    const pixels = []
    for (let row = 1; row < SAMPLES; row++) {
      for (let column = 1; column < SAMPLES; column++) {
        pixels.push(native.readPixel(o2.getCurrent().id, Math.floor(width * column / SAMPLES), Math.floor(height * row / SAMPLES))[0])
      }
    }
    return pixels.join()
  }
  const failures = []
  const builtIn = text
  // What the extension does when a sketch loads it, in sloppy mode as loadScript runs it.
  globalThis.eval("window.hydraText = { font: 'sans-serif' }; window.text = function () { throw new Error('canvas') }")
  if (text !== builtIn) failures.push('the hydra-text extension replaced text()')
  hydraText.font = 'serif'
  const filled = await sample(text('HI'))
  const outlined = await sample(strokeText('HI'))
  if (!/[1-9]/.test(filled)) failures.push('text() draws nothing')
  if (!/[1-9]/.test(outlined) || outlined === filled) failures.push('strokeText() does not draw an outline')
  return failures
}

// fetch and XMLHttpRequest of a local file, as text and as bytes.
function networkChecks () {
  const path = native.directory + '/hydra-bundle.js'
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
    if (!code.includes('hydra')) failures.push('fetch: no text')
    if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < code.length) failures.push('XMLHttpRequest: no bytes')
    return failures
  }, (error) => [`fetch: ${error.message}`])
}

// While the driver compiles new passes, each output keeps its old pass; then the newest pass wins, and a pass
// that does not compile goes to the error bar (js/main.js). programStatus() reports "compiling" here on request.
// Frames count the steps: a timer can come late, after the GPU has rendered the frames that it queued.
function pendingPassChecks () {
  if (native.shaderCompile === 'blocking') {
    console.log('pending passes: not checked, nothing compiles in the background')
    return Promise.resolve([])
  }
  const programStatus = native.programStatus
  const showError = native.showError
  const errors = []
  const failures = []
  let holding = false
  native.programStatus = (program, wait) => !wait && holding ? false : programStatus(program, wait)
  native.showError = (text) => { errors.push(text); showError(text) }
  const center = (output) => native.readPixel(output.getCurrent().id, Math.floor(width / 2), Math.floor(height / 2))
  const expect = (name, actual, expected) => {
    if (actual.join() !== expected.join()) failures.push(`${name}: ${actual} instead of ${expected}`)
  }
  const compiled = async () => {
    for (let frame = 0; frame < PENDING_MAX_FRAMES && (o2.pendingDraw || o3.pendingDraw); frame++) await frames(1)
  }
  setFunction({ name: 'checkBroken', type: 'src', inputs: [], glsl: 'return undefinedName;' })
  solid(0, 0, 1).out(o2)
  return compiled().then(async () => {
    holding = true
    solid(1, 0, 0).out(o2)
    solid(0, 1, 0).out(o2)
    checkBroken().out(o3)
    await frames(PENDING_FRAMES)
    expect('old pass while compiling', center(o2), [0, 0, 255, 255])
    holding = false
    await compiled()
    await frames(PENDING_FRAMES)
    expect('newest pass after compiling', center(o2), [0, 255, 0, 255])
    const shown = errors.some((text) => text.includes('shader could not compile'))
    if (!shown) failures.push(`shader error not shown: ${errors}`)
    return failures
  })
}

// A program goes with the pass that an eval replaced (js/regl-shim.js), so many different shaders do not add up.
async function programChecks () {
  const DIFFERENT_SHADERS = 50
  const before = hydraSynth.regl.programCount()
  for (let count = 1; count <= DIFFERENT_SHADERS; count++) {
    let chain = solid(1, 0, 0)
    for (let i = 0; i < count; i++) chain = chain.invert()
    chain.out(o3)
    for (let frame = 0; frame < PENDING_MAX_FRAMES && o3.pendingDraw; frame++) await frames(1)
  }
  await frames(PENDING_FRAMES)
  const added = hydraSynth.regl.programCount() - before
  return added > 2 ? [`programs: ${added} more after ${DIFFERENT_SHADERS} different shaders`] : []
}

const checks = Promise.all([networkChecks(), pendingPassChecks()
  .then(async (failures) => [...failures, ...await textChecks(), ...await programChecks()])])

setTimeout(() => checks.then((results) => {
  const failures = results.flat()
  const x = Math.floor(width / 2), y = Math.floor(height / 2)
  const pixels = [o0, o1].map((output) => native.readPixel(output.getCurrent().id, x, y))
  const ok = pixels.every(([r, g, b]) => r + g + b > 0) && !failures.length
  console.log(JSON.stringify({ check: ok ? 'ok' : 'failed', size: [width, height], o0: pixels[0], o1: pixels[1], failures }))
  native.exit(ok ? 0 : 1)
}), CHECK_DELAY_MS)
