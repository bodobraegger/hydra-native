// Replaces the raf-loop package: hydra-synth only calls loop(tick).start().
export default function loop (callback) {
  let running = false
  let last = 0
  const step = (now) => {
    if (!running) return
    callback(last ? now - last : 0)
    last = now
    requestAnimationFrame(step)
  }
  return {
    start () {
      running = true
      requestAnimationFrame(step)
      return this
    },
    stop () {
      running = false
      return this
    }
  }
}
