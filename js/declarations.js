// Sketch code runs in the global scope, again and again in live coding, and a second top-level const, let or class
// of the same name throws a redeclaration error there. rewriteDeclarations turns them into var, which can be
// declared again: `const` becomes `var  ` and `let` becomes `var`, so that the columns stay, and `class Name {}`
// becomes `var Name = class Name {};`. No line is added or removed, so the line numbers of errors stay.
import { endsExpression, tokenize } from './tokenize.js'

const OPENING = new Set(['(', '[', '{'])
const CLOSING = new Set([')', ']', '}'])
const DESTRUCTURING = new Set(['[', '{']) // let [a, b] = ..., let { a } = ...

// At the start of the code, after a ";" or a block, or on a new line after an expression (automatic semicolon
// insertion): after an operator or an open bracket, the next line continues the expression.
const startsStatement = (code, before, token) => !before ||
  (before.kind === 'punct' && (before.text === ';' || before.text === '}')) ||
  (endsExpression(before) && code.slice(before.end, token.start).includes('\n'))

export function rewriteDeclarations (code) {
  const changes = []
  let depth = 0 // of brackets and of the ${ } of template literals
  let classPending = false // a rewritten class whose body is still to come
  let classBody = false // inside the body of a rewritten class, which gets a ";" after its end
  const tokens = tokenize(code)
  tokens.forEach((token, index) => {
    const next = tokens[index + 1]
    if (depth === 0 && token.kind === 'name' && startsStatement(code, tokens[index - 1], token)) {
      const replace = (text) => changes.push({ start: token.start, end: token.end, text })
      if (token.text === 'const') replace('var  ')
      else if (token.text === 'let' && next && (next.kind === 'name' || DESTRUCTURING.has(next.text))) replace('var')
      else if (token.text === 'class' && next && next.kind === 'name') {
        replace(`var ${next.text} = class`)
        classPending = true
      }
    }
    if (token.kind === 'string' && token.text[0] === '}') depth-- // the end of a ${ } in a template literal
    if (token.kind === 'string' && '`}'.includes(token.text[0]) && token.text.endsWith('${')) depth++
    if (token.kind !== 'punct') return
    if (OPENING.has(token.text)) {
      if (depth === 0 && token.text === '{' && classPending) [classPending, classBody] = [false, true]
      depth++
    } else if (CLOSING.has(token.text) && --depth === 0 && classBody && token.text === '}') {
      classBody = false
      changes.push({ start: token.end, end: token.end, text: ';' })
    }
  })
  let rewritten = ''
  let copied = 0
  for (const { start, end, text } of changes) {
    rewritten += code.slice(copied, start) + text
    copied = end
  }
  return rewritten + code.slice(copied)
}
