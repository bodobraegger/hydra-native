// Checks the scanner of js/hydra-editor/Mutator.js: which numbers and transform calls the mutator may change.
// Run with `node test/mutator_test.mjs` (ctest "mutator").
import assert from 'node:assert/strict'
import { scanCode } from '../js/hydra-editor/Mutator.js'

const transforms = { osc: {}, rotate: {}, modulate: {}, color: {} }
const code = `// osc(99).rotate(98)
osc(10, 0.1, 1.5) /* 97 */
  .rotate(() => a.fft[0] * 2)
  .modulate(noise(3), [0.1, 0.2][1])
  .color(...'7', \`\${4} 96\`, /9[5]/.test('') ? 1e-3 : 0x10)
  .out(o0)
Math.sin(5)
`
const { numbers, calls } = scanCode(code, transforms)
console.log('numbers', numbers.map((number) => number.text).join(' '))
console.log('calls', calls.map((call) => call.text).join(' '))
assert.deepEqual(numbers.map((number) => number.value), [10, 0.1, 1.5, 2, 3, 0.1, 0.2, 4, 0.001, 16, 5])
assert.deepEqual(calls.map((call) => call.text), ['rotate', 'modulate', 'color'])
for (const number of numbers) assert.equal(code.slice(number.start, number.end), number.text)
console.log('mutator_test: ok')
