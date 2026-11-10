// The toolbar actions of the web editor that need JavaScript: random examples and the mutator.
// src/ui.c calls the window.__* functions through the `call` host function.
import Mutator from './hydra-editor/Mutator.js'
import examples from './hydra-editor/examples.json'

const BASE64_ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'

// QuickJS has no atob().
function decodeBase64 (text) {
  let bits = 0
  let value = 0
  let output = ''
  for (const character of text.replace(/[^A-Za-z0-9+/]/g, '')) {
    value = (value << 6) | BASE64_ALPHABET.indexOf(character)
    bits += 6
    if (bits >= 8) {
      bits -= 8
      output += String.fromCharCode((value >> bits) & 0xff)
    }
  }
  return output
}

let exampleIndex = -1

// Like setRandomSketch() of the web editor: never the same example twice in a row.
window.__randomExample = () => {
  let index = Math.floor(Math.random() * examples.length)
  while (index === exampleIndex && examples.length > 1) index = Math.floor(Math.random() * examples.length)
  exampleIndex = index
  return decodeURIComponent(decodeBase64(examples[index].code))
}

let mutatorText = ''
const mutatorEditor = { cm: { getValue: () => mutatorText, setValue: (text) => { mutatorText = text } } }
let mutator

function mutate (code, changeTransform) {
  if (!mutator) mutator = new Mutator(mutatorEditor)
  mutatorText = code
  mutator.mutate({ reroll: false, changeTransform })
  return window.__format(mutatorText)
}

window.__mutate = (code) => mutate(code, false)
window.__mutateTransform = (code) => mutate(code, true)
window.__mutateUndo = (code) => {
  if (!mutator) return code
  mutatorText = code
  mutator.doUndo()
  return mutatorText
}
