// Replaces the meyda package: hydra-synth's lib/audio.js calls createMeydaAnalyzer(...).get()
// and reads features.loudness. src/audio.c computes the same feature from PulseAudio/PipeWire input.
export default {
  createMeydaAnalyzer () {
    native.audioStart()
    return { get: () => native.audioFeatures(), start () {}, stop () {} }
  }
}
