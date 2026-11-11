// Signatures of the hydra functions that are not in the glsl list of hydra-synth, for the signature help of
// src/ui.c. out, render and setResolution come from types/index.d.ts of hydra-synth pull request #169 (by the
// author of hydra-native), written as there; `out` takes an output such as o1 instead of the number of that file.
// The others are in the same notation: the methods of hydra-synth that the file lacks, and the sources of
// hydra-native (initMonitor, initArea, initWindow, initCommand). Each entry is "kind name(parameters)".
const API_SIGNATURES = [
  'output out(output?: HydraOutput)',
  'synth render(source?: HydraSource)',
  'synth setResolution(width: number, height: number)',
  'synth hush()',
  'synth loadScript(url: string)',
  'synth setFunction(definition: object)',
  'synth screencap()',
  'source initCam(index?: number, params?: object)',
  'source initScreen(index?: number, params?: object)',
  'source initVideo(url: string, params?: object)',
  'source initImage(url: string, params?: object)',
  'source initStream(streamName: string, params?: object)',
  'source initCanvas(width?: number, height?: number)',
  'source initMonitor(index?: number, params?: object)',
  'source initArea(x: number, y: number, areaWidth: number, areaHeight: number, params?: object)',
  'source initWindow(delay?: number, params?: object)',
  'source initCommand(command: string, params?: object)',
  'source clear()',
  'audio setBins(numBins: number)',
  'audio setSmooth(smooth: number)',
  'audio setScale(scale: number)',
  'audio setCutoff(cutoff: number)',
  'audio show()',
  'audio hide()'
]

// The lines of __functionSignatures (js/main.js): "name<TAB>kind<TAB>input:type:default".
export const apiSignatureLines = () => API_SIGNATURES.map((entry) => {
  const [, kind, name, parameters] = entry.match(/^(\w+) (\w+)\((.*)\)$/)
  const inputs = parameters ? parameters.split(', ').map((parameter) => parameter.replace(': ', ':') + ':') : []
  return [name, kind, ...inputs].join('\t')
})
