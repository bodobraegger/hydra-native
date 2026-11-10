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
  return mutatorText
}

window.__mutate = (code) => mutate(code, false)
window.__mutateTransform = (code) => mutate(code, true)
window.__mutateUndo = (code) => {
  if (!mutator) return code
  mutatorText = code
  mutator.doUndo()
  return mutatorText
}

const WEB_EDITOR_URL = 'https://hydra.ojack.xyz/'
const GALLERY_LICENSE = '// licensed with CC BY-NC-SA 4.0 https://creativecommons.org/licenses/by-nc-sa/4.0/'

// QuickJS has no btoa(); the input is ASCII (the output of encodeURIComponent).
function encodeBase64 (text) {
  let output = ''
  for (let i = 0; i < text.length; i += 3) {
    const bytes = [text.charCodeAt(i), text.charCodeAt(i + 1), text.charCodeAt(i + 2)]
    const value = (bytes[0] << 16) | ((bytes[1] || 0) << 8) | (bytes[2] || 0)
    output += BASE64_ALPHABET[(value >> 18) & 63] + BASE64_ALPHABET[(value >> 12) & 63]
    output += i + 1 < text.length ? BASE64_ALPHABET[(value >> 6) & 63] : '='
    output += i + 2 < text.length ? BASE64_ALPHABET[value & 63] : '='
  }
  return output
}

// The URL of "save to URL" in the web editor: gallery.js encodes btoa(encodeURIComponent(code)), and
// URLSearchParams escapes the base64 characters + / and =.
window.__webEditorUrl = () => WEB_EDITOR_URL
window.__urlForCode = (code) => `${WEB_EDITOR_URL}?code=${encodeURIComponent(encodeBase64(encodeURIComponent(code)))}`

// ---------------------------------------------------------------- sharing with the gallery

// The query of superagent in gallery.js: encodeURIComponent, and a null value as a bare key.
const query = (entries) => entries.map(([key, value]) =>
  value === null ? encodeURIComponent(key) : `${encodeURIComponent(key)}=${encodeURIComponent(value)}`).join('&')

const asciiBytes = (text) => Uint8Array.from(text, (character) => character.charCodeAt(0))

// shareSketch of gallery.js in the web editor: POST /sketch?code=...&parent answers the sketch id, then POST
// /image?url=...&sketch_id=...&name=... sends the screenshot as the multipart field "previewImage". src/main.c
// calls this with the PNG of the next frame; the answer goes to native.shareDone(ok, sketch URL or error).
window.__shareSketch = (server, code, image, name) => {
  const base64 = encodeBase64(encodeURIComponent(code))
  const checked = (response) => {
    if (!response.ok) throw new Error(`HTTP ${response.status} from ${response.url}`)
    return response.text()
  }
  fetch(`${server}/sketch?${query([['code', base64], ['parent', null]])}`, { method: 'POST' })
    .then(checked)
    .then((answer) => {
      const sketchId = answer.trim()
      const url = `${WEB_EDITOR_URL}?${query([['sketch_id', sketchId], ['code', base64]])}`
      const boundary = `hydra-native-${Date.now().toString(16)}`
      const head = asciiBytes(`--${boundary}\r\nContent-Disposition: form-data; name="previewImage"; filename="blob"\r\n` +
        'Content-Type: image/png\r\n\r\n')
      const tail = asciiBytes(`\r\n--${boundary}--\r\n`)
      const body = new Uint8Array(head.length + image.byteLength + tail.length)
      body.set(head)
      body.set(new Uint8Array(image), head.length)
      body.set(tail, head.length + image.byteLength)
      return fetch(`${server}/image?${query([['url', url], ['sketch_id', sketchId], ['name', name]])}`, {
        method: 'POST', headers: { 'Content-Type': `multipart/form-data; boundary=${boundary}` }, body
      }).then(checked).then(() => url)
    })
    .then((url) => native.shareDone(true, url), (error) => native.shareDone(false, String(error.message || error)))
}

// The gallery server for reading sketches: --gallery-server, else the server of the deployed web editor.
const DEFAULT_GALLERY_SERVER = 'https://api.hydrasynth.xyz'

// GET <server>/sketchById?sketch_id=<id> answers a JSON list of sketches, as in gallery.js.
function sketchFromServer (sketchId) {
  const server = native.galleryServer || DEFAULT_GALLERY_SERVER
  try {
    const sketches = JSON.parse(native.fetchSync(`${server}/sketchById?sketch_id=${encodeURIComponent(sketchId)}`))
    return sketches.length > 0 ? sketches[0] : null
  } catch (error) {
    console.warn(`gallery: cannot get sketch ${sketchId}: ${error}`)
    return null
  }
}

function queryParameter (query, name) {
  for (const pair of query.split('&')) {
    const [key, ...rest] = pair.split('=')
    if (decodeURIComponent(key) === name) return decodeURIComponent(rest.join('=').replace(/\+/g, ' '))
  }
  return null
}

// Opens a URL of the web editor (or only its "?..." part): ?code=<base64>, or ?sketch_id= of an example.
// Returns the code; the line "// show-code: false" first when the URL asks to hide the code. Throws without code.
window.__codeFromUrl = (url) => {
  const query = url.includes('?') ? url.slice(url.indexOf('?') + 1).split('#')[0] : url
  const sketchId = queryParameter(query, 'sketch_id')
  // Like setSketchFromURL() of gallery.js: an example, else the sketch of the gallery server, else ?code=.
  const sketch = sketchId && (examples.find((item) => item.sketch_id === sketchId) || sketchFromServer(sketchId))
  const base64 = sketch ? sketch.code : queryParameter(query, 'code')
  if (!base64) throw new Error(`no code in ${url}`)
  let code = decodeURIComponent(decodeBase64(base64))
  if (sketch && !code.includes(GALLERY_LICENSE)) code = `${GALLERY_LICENSE}\n${code}\n`
  const showCode = queryParameter(query, 'showCode') || queryParameter(query, 'show-code')
  return showCode === 'false' ? `// show-code: false\n${code}` : code
}
