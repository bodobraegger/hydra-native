// Runs with `make check`: renders a chain into o0 and a feedback chain into o1,
// reads pixels back and exits with status 0 when both outputs contain color.
const CHECK_DELAY_MS = 500

osc(10, 0.1, 1.2).rotate(0.3).modulate(noise(3), 0.2).out(o0)
src(o1).scale(1.01).blend(shape(4, 0.5).color(1, 0.5, 0), 0.5).out(o1)
render(o0)

setTimeout(() => {
  const x = Math.floor(width / 2), y = Math.floor(height / 2)
  const pixels = [o0, o1].map((output) => native.readPixel(output.getCurrent().id, x, y))
  const ok = pixels.every(([r, g, b]) => r + g + b > 0)
  console.log(JSON.stringify({ check: ok ? 'ok' : 'failed', size: [width, height], o0: pixels[0], o1: pixels[1] }))
  native.exit(ok ? 0 : 1)
}, CHECK_DELAY_MS)
