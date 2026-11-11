// The mutator of the hydra web editor (https://github.com/hydra-synth/hydra, src/views/editor/randomizer/Mutator.js,
// commit 680273b), AGPL-3.0-or-later, by the hydra authors. See THIRD_PARTY.md.
// Changes for hydra-native: the web editor parses the code with acorn and writes it again with astring; here a
// scanner finds the same places in the text (scanCode below, with js/tokenize.js), so the code keeps its format
// and its comments. It changes only numbers (the web editor also changed strings and booleans into numbers) and
// only the calls of hydra transforms (the web editor threw an error for another method call). A syntax error stops
// the mutation, as the parser of the web editor did.
import glslFunctions from 'hydra-synth/src/glsl/glsl-functions.js'
import { EXPRESSION_KEYWORDS, tokenize } from '../tokenize.js'
import { rewriteDeclarations } from '../declarations.js'

const MAX_TRIES = 6 // tries of a mutation until the code evaluates without an error
const AsyncFunction = (async () => {}).constructor

// The numbers and the calls of hydra transforms in `text`, in the order of the text. A number that is the whole
// index of a member (a.fft[0]) does not count, as in the web editor.
export function scanCode (text, transforms) {
  const tokens = tokenize(text)
  const numbers = []
  const calls = []
  const isPunct = (token, character) => token && token.kind === 'punct' && token.text === character
  tokens.forEach((token, index) => {
    const before = tokens[index - 1]
    if (token.kind === 'number' && !token.text.endsWith('n')) {
      const beforeBracket = tokens[index - 2]
      const memberIndex = isPunct(before, '[') && isPunct(tokens[index + 1], ']') && beforeBracket &&
        (beforeBracket.kind === 'name' ? !EXPRESSION_KEYWORDS.has(beforeBracket.text)
          : [')', ']'].includes(beforeBracket.text))
      if (!memberIndex) numbers.push({ ...token, value: Number(token.text.replace(/_/g, '')) })
    } else if (token.kind === 'name' && isPunct(before, '.') && isPunct(tokens[index + 1], '(') &&
      token.text !== 'out' && transforms[token.text]) {
      calls.push(token)
    }
  })
  return { numbers, calls }
}

export default class Mutator {
  constructor () {
    this.undoStack = []
    this.initialVector = []
    // Without a prototype, so that a method name such as toString is no transform.
    this.funcTab = Object.create(null)
    this.transMap = Object.create(null)
    for (const transform of glslFunctions()) {
      this.transMap[transform.name] = transform
      if (this.funcTab[transform.type] === undefined) this.funcTab[transform.type] = []
      this.funcTab[transform.type].push(transform)
    }
  }

  // Changes one number of `text`, or the name of one transform, runs the new code and returns it. A change
  // that fails to run is replaced by another one, MAX_TRIES times.
  mutate (text, changeTransform) {
    new AsyncFunction(text) // eslint-disable-line no-new
    this.undoStack.push(text)
    let code = text
    for (let tries = 0; tries < MAX_TRIES && !this.run(code = this.transform(text, changeTransform)); tries++);
    return code
  }

  // Runs and returns the code before the last mutation, or `text` when there is none.
  undo (text) {
    const previous = this.undoStack.pop()
    if (previous === undefined) return text
    this.run(previous)
    return previous
  }

  run (code) {
    try {
      globalThis.eval(rewriteDeclarations(code))
      return true
    } catch (error) {
      console.log(`mutator: ${error}`)
      return false
    }
  }

  transform (text, changeTransform) {
    const { numbers, calls } = scanCode(text, this.transMap)
    if (numbers.length !== this.initialVector.length) this.initialVector = numbers.map((number) => number.value)
    const change = changeTransform ? this.glitchTrans(calls) : this.glitchLiteral(numbers)
    return change ? text.slice(0, change.start) + change.text + text.slice(change.end) : text
  }

  // A random value between 0 and twice the value that the number had at first (0.5 for a 0).
  glitchLiteral (numbers) {
    const litx = Math.floor(Math.random() * numbers.length)
    const number = numbers[litx]
    if (!number) return null
    const glitched = Math.round(Math.random() * (this.initialVector[litx] || 0.5) * 2 * 1000) / 1000
    console.log('Literal: ' + litx + ' changed from: ' + number.text + ' to: ' + glitched)
    return { start: number.start, end: number.end, text: String(glitched) }
  }

  glitchTrans (calls) {
    const call = calls[Math.floor(Math.random() * calls.length)]
    if (!call) return null
    const others = this.funcTab[this.transMap[call.text].type]
    const become = others[Math.floor(Math.random() * others.length)].name
    if (call.text === 'modulate' && become === 'modulateScrollX') return null // as in the web editor
    console.log('Function: ' + call.text + ' changed to: ' + become)
    return { start: call.start, end: call.end, text: become }
  }
}
