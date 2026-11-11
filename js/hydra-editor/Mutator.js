// The mutator of the hydra web editor (https://github.com/hydra-synth/hydra, src/views/editor/randomizer/Mutator.js,
// commit 680273b), AGPL-3.0-or-later, by the hydra authors. See THIRD_PARTY.md.
// Changes for hydra-native: the web editor parses the code with acorn and writes it again with astring; here a
// scanner finds the same places in the text (scanCode below), so the code keeps its format and its
// comments. It changes only numbers (the web editor also changed strings and booleans into numbers) and only the
// calls of hydra transforms (the web editor threw an error for another method call). A syntax error stops the
// mutation, as the parser of the web editor did.
import glslFunctions from 'hydra-synth/src/glsl/glsl-functions.js'

const MAX_TRIES = 6 // tries of a mutation until the code evaluates without an error
const AsyncFunction = (async () => {}).constructor
// After these words a "/" starts a regular expression, and a "[" starts an array.
const EXPRESSION_KEYWORDS = new Set(['return', 'typeof', 'instanceof', 'in', 'of', 'new', 'delete', 'void', 'throw',
  'case', 'do', 'else', 'yield', 'await'])
const NUMBER = /0[xX][\da-fA-F_]+n?|0[bB][01_]+n?|0[oO][0-7_]+n?|(?:\d[\d_]*\.?[\d_]*|\.\d[\d_]*)(?:[eE][+-]?\d[\d_]*)?n?/y
const NAME = /[A-Za-z_$\u00a0-\uffff][\w$\u00a0-\uffff]*/y

// The tokens of `text` that the mutator needs: numbers, names and single punctuation characters, with their
// offsets. Strings, template literals and regular expressions are "string" tokens; comments are skipped.
function tokenize (text) {
  const tokens = []
  const templateDepths = [] // the brace depth of each open ${ in a template literal
  let depth = 0
  let i = 0
  const last = () => tokens[tokens.length - 1]
  const endsExpression = (token) => token && (token.kind === 'number' || token.kind === 'string' ||
    (token.kind === 'name' && !EXPRESSION_KEYWORDS.has(token.text)) || [')', ']', '}'].includes(token.text))
  const add = (kind, start) => tokens.push({ kind, start, end: i, text: text.slice(start, i) })
  // From inside a template literal to its end, or to a ${, which opens a brace.
  const templateRest = (start) => {
    while (i < text.length && text[i] !== '`') {
      if (text[i] === '\\') i++
      else if (text[i] === '$' && text[i + 1] === '{') {
        i += 2
        templateDepths.push(depth++)
        return add('string', start)
      }
      i++
    }
    i++
    add('string', start)
  }
  while (i < text.length) {
    const c = text[i]
    const start = i
    if (/\s/.test(c)) i++
    else if (c === '/' && text[i + 1] === '/') while (i < text.length && text[i] !== '\n') i++
    else if (c === '/' && text[i + 1] === '*') i = text.indexOf('*/', i + 2) < 0 ? text.length : text.indexOf('*/', i + 2) + 2
    else if (c === '"' || c === "'") {
      for (i++; i < text.length && text[i] !== c && text[i] !== '\n'; i++) if (text[i] === '\\') i++
      i++
      add('string', start)
    } else if (c === '`') {
      i++
      templateRest(start)
    } else if (c === '}' && templateDepths.length && templateDepths[templateDepths.length - 1] === depth - 1) {
      templateDepths.pop()
      depth--
      i++
      templateRest(start)
    } else if (c === '/' && !endsExpression(last())) {
      for (let inClass = false; ++i < text.length && text[i] !== '\n';) {
        if (text[i] === '\\') i++
        else if (text[i] === '[') inClass = true
        else if (text[i] === ']') inClass = false
        else if (text[i] === '/' && !inClass) break
      }
      for (i++; /\w/.test(text[i] || ''); i++);
      add('string', start)
    } else if ((NUMBER.lastIndex = i, NUMBER.test(text) && /[\d.]/.test(c))) {
      i = NUMBER.lastIndex
      add('number', start)
    } else if ((NAME.lastIndex = i, NAME.test(text))) {
      i = NAME.lastIndex
      add('name', start)
    } else {
      if (c === '{') depth++
      if (c === '}') depth--
      i++
      add('punct', start)
    }
  }
  return tokens
}

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
    this.funcTab = {}
    this.transMap = {}
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
      globalThis.eval(code)
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
