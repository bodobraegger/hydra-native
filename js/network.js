import { EventTarget } from './element.js'

// fetch() and XMLHttpRequest for sketches and libraries (p5.js loadFont, loadJSON): http(s) requests go through
// curl (src/fetch.c) with method, headers and body, and give the status and headers of the response. The CORS and
// mixed-content rules of a web page do not apply. Other URLs are files relative to the working directory.

const URL_PATTERN = /^https?:\/\//
const STATUS_OK = 200
const HEADER_END = [13, 10, 13, 10] // \r\n\r\n
const MAX_PARALLEL_REQUESTS = 32 // src/fetch.c runs at most 64 curl processes; the others wait here
const READY_STATES = { UNSENT: 0, OPENED: 1, HEADERS_RECEIVED: 2, LOADING: 3, DONE: 4 }

// Callbacks of native.fetchStart() downloads by id, also for js/main.js, js/l10n.js and js/extensions.js.
export const pendingFetches = new Map()

// src/main.c calls this when a download of native.fetchStart() ends.
window.__fetchDone = (id, ok, text, buffer) => {
  const callback = pendingFetches.get(id)
  pendingFetches.delete(id)
  if (callback) callback(ok, text, buffer)
}

class Headers {
  constructor (init) {
    this.values = new Map()
    const entries = init instanceof Headers ? [...init.values] : Array.isArray(init) ? init : Object.entries(init || {})
    for (const [name, value] of entries) this.append(name, value)
  }

  append (name, value) {
    const key = String(name).toLowerCase()
    this.values.set(key, this.values.has(key) ? `${this.values.get(key)}, ${value}` : String(value))
  }

  set (name, value) { this.values.set(String(name).toLowerCase(), String(value)) }
  get (name) { return this.values.get(String(name).toLowerCase()) ?? null }
  has (name) { return this.values.has(String(name).toLowerCase()) }
  delete (name) { this.values.delete(String(name).toLowerCase()) }
  forEach (callback, self) { for (const [name, value] of this.values) callback.call(self, value, name, this) }
  entries () { return this.values.entries() }
  [Symbol.iterator] () { return this.values.entries() }
}

class Response {
  constructor (url, readText, readBuffer, { status = STATUS_OK, statusText = 'OK', headers = new Headers(), redirected = false,
    type = 'basic' } = {}) {
    Object.assign(this, { url, status, statusText, headers, redirected, ok: status >= 200 && status < 300, type })
    this.readText = readText
    this.readBuffer = readBuffer
  }

  text () { return Promise.resolve().then(this.readText) }
  json () { return this.text().then(JSON.parse) }
  arrayBuffer () { return Promise.resolve().then(this.readBuffer) }
}

function indexOfHeaderEnd (bytes, start) {
  for (let i = start; i + 3 < bytes.length; i++) if (HEADER_END.every((byte, k) => bytes[i + k] === byte)) return i
  return -1
}

// Splits the output of curl -D - into the last header block (after redirects and 100 Continue) and the body.
function parseResponse (url, text, buffer, redirected) {
  const bytes = new Uint8Array(buffer)
  let offset = 0, status = 0, statusText = '', headers = new Headers(), blocks = 0
  while (bytes[offset] === 72 && String.fromCharCode(...bytes.subarray(offset, offset + 5)) === 'HTTP/') { // "H"
    const end = indexOfHeaderEnd(bytes, offset)
    if (end < 0) break
    const [statusLine, ...lines] = String.fromCharCode(...bytes.subarray(offset, end)).split('\r\n')
    const match = /^HTTP\/[\d.]+ (\d+) ?(.*)$/.exec(statusLine)
    status = match ? Number(match[1]) : 0
    statusText = match ? match[2] : ''
    headers = new Headers(lines.map((line) => [line.slice(0, line.indexOf(':')), line.slice(line.indexOf(':') + 1).trim()]))
    offset = end + HEADER_END.length
    blocks++
  }
  const body = buffer.slice(offset)
  return new Response(url, () => text.slice(offset), () => body.slice(0), { status, statusText, headers, redirected: redirected || blocks > 1 })
}

const waitingRequests = []
let runningRequests = 0

function startRequest (start) {
  if (runningRequests >= MAX_PARALLEL_REQUESTS) return waitingRequests.push(start)
  runningRequests++
  start()
}

function finishRequest () {
  runningRequests--
  if (waitingRequests.length) startRequest(waitingRequests.shift())
}

function send (url, method, headers, data) {
  return new Promise((resolve, reject) => startRequest(() => {
    const lines = []
    headers.forEach((value, name) => lines.push(`${name}:${value ? ' ' + value : ''}`))
    try {
      pendingFetches.set(native.httpRequest(url, method, lines, data), (ok, text, buffer) => {
        finishRequest()
        if (ok) resolve(parseResponse(url, text, buffer, false))
        else reject(new TypeError('Failed to fetch'))
      })
    } catch (error) {
      finishRequest()
      reject(new TypeError('Failed to fetch'))
    }
  }))
}

function request (url, method, headerInit, body) {
  const data = body === undefined || body === null ? null
    : typeof body === 'string' || body instanceof ArrayBuffer || ArrayBuffer.isView(body) ? body : String(body)
  const headers = new Headers(headerInit)
  // The content type of the browser: text for a string, none for bytes ("Content-Type:" removes the one of curl).
  if (data !== null && !headers.has('content-type')) headers.set('content-type', typeof data === 'string' ? 'text/plain;charset=UTF-8' : '')
  return send(url, method, headers, data)
}

function readLocal (url) {
  return new Promise((resolve, reject) => {
    const path = url.replace(/^file:\/\//, '')
    try {
      native.readFile(path, true) // fails here for a missing file, as a network error
    } catch (error) {
      return reject(new TypeError(`Failed to fetch ${url}: ${error.message}`))
    }
    resolve(new Response(url, () => native.readFile(path), () => native.readFile(path, true)))
  })
}

window.fetch = (input, options = {}) => {
  const url = String(input?.url ?? input)
  const method = String(options.method || input?.method || 'GET').toUpperCase()
  if (!URL_PATTERN.test(url)) return readLocal(url)
  return request(url, method, options.headers || input?.headers, options.body)
}
window.Headers = Headers
window.Response = Response

class XMLHttpRequest extends EventTarget {
  constructor () {
    super()
    Object.assign(this, { readyState: READY_STATES.UNSENT, responseType: '', response: null, responseText: '', responseURL: '' })
    Object.assign(this, { status: 0, statusText: '', timeout: 0, withCredentials: false, upload: new EventTarget() })
  }

  open (method, url) {
    this.method = String(method).toUpperCase()
    this.url = String(url)
    this.requestHeaders = new Headers()
    this.change(READY_STATES.OPENED)
  }

  setRequestHeader (name, value) { this.requestHeaders.append(name, value) }
  overrideMimeType () {}
  getResponseHeader (name) { return this.responseHeaders?.get(name) ?? null }
  getAllResponseHeaders () {
    let text = ''
    this.responseHeaders?.forEach((value, name) => { text += `${name}: ${value}\r\n` })
    return text
  }
  abort () { this.aborted = true }

  change (state) {
    this.readyState = state
    this.event('readystatechange')
  }

  event (type) {
    const event = { type, target: this, currentTarget: this, lengthComputable: false, loaded: 0, total: 0 }
    this.dispatchEvent(event)
  }

  send (body) {
    window.fetch(this.url, { method: this.method, headers: this.requestHeaders, body }).then(async (response) => {
      if (this.aborted) return
      const binary = this.responseType === 'arraybuffer' || this.responseType === 'blob'
      const body = binary ? await response.arrayBuffer() : await response.text()
      Object.assign(this, { status: response.status, statusText: response.statusText, responseURL: this.url })
      this.responseHeaders = response.headers
      this.response = this.responseType === 'json' ? JSON.parse(body) : body
      if (!binary && this.responseType !== 'json') this.responseText = body
      this.change(READY_STATES.DONE)
      this.event('load')
      this.event('loadend')
    }, () => {
      if (this.aborted) return
      this.change(READY_STATES.DONE)
      this.event('error')
      this.event('loadend')
    })
  }
}
Object.assign(XMLHttpRequest, READY_STATES)
window.XMLHttpRequest = XMLHttpRequest
