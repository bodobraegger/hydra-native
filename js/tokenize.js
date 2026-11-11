// A JavaScript tokenizer for the mutator (js/hydra-editor/Mutator.js) and the rewrite of declarations
// (js/declarations.js).

// After these words a "/" starts a regular expression, and a "[" starts an array.
export const EXPRESSION_KEYWORDS = new Set(['return', 'typeof', 'instanceof', 'in', 'of', 'new', 'delete', 'void',
  'throw', 'case', 'do', 'else', 'yield', 'await'])
const PREFIXED_NUMBER = /0[xX][\da-fA-F_]+n?|0[bB][01_]+n?|0[oO][0-7_]+n?/
const DECIMAL_NUMBER = /(?:\d[\d_]*\.?[\d_]*|\.\d[\d_]*)(?:[eE][+-]?\d[\d_]*)?n?/
const NUMBER = new RegExp(`${PREFIXED_NUMBER.source}|${DECIMAL_NUMBER.source}`, 'y')
const NAME = /[A-Za-z_$\u00a0-\uffff][\w$\u00a0-\uffff]*/y

// Whether `token` can be the last token of an expression, so that a "/" after it divides. A "}" ends an object
// literal there, and a block is followed by a new statement.
const endsExpression = (token) => Boolean(token) && (token.kind === 'number' || token.kind === 'string' ||
  (token.kind === 'name' && !EXPRESSION_KEYWORDS.has(token.text)) || [')', ']'].includes(token.text) ||
  (token.text === '}' && !token.block))

// The tokens of `text`: numbers, names and single punctuation characters, with their offsets. Strings, template
// literals and regular expressions are "string" tokens; comments are skipped. A "}" of a block has `block`.
export function tokenize (text) {
  const tokens = []
  const templateDepths = [] // the brace depth of each open ${ in a template literal
  const blocks = [] // for each open "{", whether it opens a block instead of an object literal
  let depth = 0
  let i = 0
  const last = () => tokens[tokens.length - 1]
  // After these a "{" opens a block: a statement, a body (after ")", "=>", a class name, else, do, try, finally).
  const opensBlock = (token) => !token || [';', '{', '}', ')'].includes(token.text) ||
    (token.text === '>' && text[token.start - 1] === '=') ||
    (token.kind === 'name' && (!EXPRESSION_KEYWORDS.has(token.text) || ['else', 'do'].includes(token.text)))
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
    else if (c === '/' && text[i + 1] === '*') {
      const end = text.indexOf('*/', i + 2)
      i = end < 0 ? text.length : end + 2
    }
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
      const block = c === '{' ? opensBlock(last()) : c === '}' && blocks.pop()
      if (c === '{') {
        depth++
        blocks.push(block)
      }
      if (c === '}') depth--
      i++
      add('punct', start)
      if (c === '}' && block) last().block = true
    }
  }
  return tokens
}
