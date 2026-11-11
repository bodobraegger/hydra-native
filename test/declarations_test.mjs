// Checks the rewrite of js/declarations.js: top-level const, let and class of sketch code can be evaluated again,
// and other code stays as it is. Run with `node test/declarations_test.mjs` (ctest "declarations").
import assert from 'node:assert/strict'
import vm from 'node:vm'
import { rewriteDeclarations } from '../js/declarations.js'

// Evaluates each code in the same global scope, as hydra-native evaluates sketch code.
const evaluateAll = (codes) => {
  const context = vm.createContext({})
  return codes.map((code) => vm.runInContext(rewriteDeclarations(code), context))
}

const plain = vm.createContext({})
vm.runInContext('const a = 1', plain)
assert.throws(() => vm.runInContext('const a = 1', plain), /already been declared/, 'without the rewrite, it throws')
assert.equal(rewriteDeclarations('const a = 1'), 'var   a = 1')
assert.deepEqual(evaluateAll(['const a = 1; a', 'const a = 2; a']), [1, 2])
assert.deepEqual(evaluateAll(['let b = 1; b', 'const b = 2; b']), [1, 2])
assert.equal(evaluateAll(['class Foo {} osc = 1', 'class Foo { value () { return 3 } }\nnew Foo().value()'])[1], 3)

const untouched = [
  'function f () { const x = 1; let y = 2; class Z {} }',
  'for (const x of [1, 2]) {}\nfor (let i = 0; i < 2; i++) {}',
  'if (true) { const inner = 1 }',
  'x = `const ${1 + 1} let`',
  '// const a = 1\n/* let b = 2 */ x = 1',
  "x = 'const a = 1'; y = /const/.test('let')",
  'let = 5; x = let',
  'x =\nclass Foo {}',
  'x = 5 -\nlet [0]',
]
for (const code of untouched) assert.equal(rewriteDeclarations(code), code, code)
// A ${ } of a template literal is no statement start, and the code after the template counts again.
assert.equal(rewriteDeclarations('x = `${(() => { const a = 1; return a })()}`\nconst b = 2'),
  'x = `${(() => { const a = 1; return a })()}`\nvar   b = 2')

assert.equal(rewriteDeclarations('x++\nconst y = 1'), 'x++\nvar   y = 1')
// A "/" after a block starts a regular expression, after an object literal it divides.
assert.equal(rewriteDeclarations("if (x) { y() }\n/'/.test(s); const c = 2"),
  "if (x) { y() }\n/'/.test(s); var   c = 2")
assert.equal(rewriteDeclarations("f = () => {}\n/'/.test(s); const c = 2"), "f = () => {}\n/'/.test(s); var   c = 2")
assert.equal(rewriteDeclarations("x = {} / 2; y = '/'; const c = 2"), "x = {} / 2; y = '/'; var   c = 2")

// An error has the same line and column with and without the rewrite.
const position = (code) => {
  try {
    vm.runInContext(code, vm.createContext({}), { filename: 'sketch.js' })
  } catch (error) {
    return error.stack.match(/sketch\.js:(\d+:\d+)/)[1]
  }
}
const failing = 'const speed = 2\nlet x = speed; const y = x.missing.value'
console.log('error position', position(failing), position(rewriteDeclarations(failing)))
assert.equal(position(rewriteDeclarations(failing)), position(failing))
console.log('declarations_test: ok')
