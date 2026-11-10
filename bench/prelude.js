// Stubs that let the repo sketches run unchanged in a benchmark:
// no extensions, no screen capture, a synthetic audio spectrum.
loadScript = () => Promise.resolve()
canvas = { setRelativeSize () {} }
for (const s of [s0, s1, s2, s3]) { s.initScreen = () => {}; s.initCam = () => {} }
if (typeof a === 'undefined' || !a.fft) a = {}
Object.assign(a, {
  fft: [0, 0, 0, 0, 0],
  show () {}, hide () {}, setBins () {}, setSmooth () {}, setScale () {}, setMax () {}, setCutoff () {}
})
numBins = 4
update = () => { for (let i = 0; i < a.fft.length; i++) a.fft[i] = 0.5 + 0.5 * Math.sin(time * (i + 1)) }
am = (c = 0.01, v = 0.1, i = 0) => c + v * a.fft[i]
