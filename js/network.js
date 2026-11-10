import { EventTarget } from './element.js'

// fetch() and XMLHttpRequest for sketches and libraries (p5.js loadFont, loadJSON): http(s) requests go through
// curl (src/fetch.c) with method, headers and body, and give the status and headers of the response. As for the
// page https://hydra.ojack.xyz, mixed content and CORS apply. Other URLs are files relative to the working
// directory, the native counterpart of the files of the page, so they count as the same origin.

const URL_PATTERN = /^https?:\/\//
const STATUS_OK = 200
const HEADER_END = [13, 10, 13, 10] // \r\n\r\n
const PAGE_ORIGIN = 'https://hydra.ojack.xyz'
const SIMPLE_METHODS = ['GET', 'HEAD', 'POST']
const SAFELISTED_REQUEST_HEADERS = ['accept', 'accept-language', 'content-language', 'content-type', 'range']
const SIMPLE_CONTENT_TYPES = ['application/x-www-form-urlencoded', 'multipart/form-data', 'text/plain']
const SAFELISTED_RESPONSE_HEADERS = ['cache-control', 'content-language', 'content-length', 'content-type', 'expires',
  'last-modified', 'pragma']
const LOOPBACK_HOST = /^(localhost|127(\.\d+){3}|\[::1\])(:\d+)?$/i
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

function urlOrigin (url) {
  const [, scheme, host] = /^(https?):\/\/([^/?#]*)/i.exec(url)
  const defaultPort = scheme.toLowerCase() === 'https' ? ':443' : ':80'
  return `${scheme.toLowerCase()}://${host.toLowerCase().replace(defaultPort, '')}`
}

// Like Chrome: the request fails with a TypeError, and the console tells why.
function blocked (url, reason) {
  console.error(`Access to fetch at '${url}' from origin '${PAGE_ORIGIN}' has been blocked by CORS policy: ${reason}`)
  return new TypeError('Failed to fetch')
}

const listed = (value, name) => (value || '').split(',').map((item) => item.trim().toLowerCase()).some((item) => item === '*' || item === name)

// The CORS check of a response (the final one after redirects) for the page origin.
function corsError (response) {
  const allowed = response.headers.get('access-control-allow-origin')
  if (allowed === '*' || allowed === PAGE_ORIGIN) return null
  return allowed ? `The 'Access-Control-Allow-Origin' header has a value '${allowed}' that is not equal to the supplied origin.`
    : "No 'Access-Control-Allow-Origin' header is present on the requested resource."
}

// A cross-origin response shows only the safelisted headers and those of Access-Control-Expose-Headers.
function exposeHeaders (response) {
  const exposed = response.headers.get('access-control-expose-headers')
  const headers = new Headers()
  response.headers.forEach((value, name) => {
    if (SAFELISTED_RESPONSE_HEADERS.includes(name) || listed(exposed, name)) headers.set(name, value)
  })
  response.headers = headers
  return response
}

async function request (url, method, headerInit, body, mode) {
  if (/^http:/i.test(url) && !LOOPBACK_HOST.test(/^http:\/\/([^/?#]*)/i.exec(url)[1])) {
    console.error(`Mixed Content: The page at '${PAGE_ORIGIN}/' was loaded over HTTPS, but requested an insecure resource '${url}'. This request has been blocked; the content must be served over HTTPS.`)
    throw new TypeError('Failed to fetch')
  }
  const data = body === undefined || body === null ? null
    : typeof body === 'string' || body instanceof ArrayBuffer || ArrayBuffer.isView(body) ? body : String(body)
  const headers = new Headers(headerInit)
  // The content type of the browser: text for a string, none for bytes ("Content-Type:" removes the one of curl).
  if (data !== null && !headers.has('content-type')) headers.set('content-type', typeof data === 'string' ? 'text/plain;charset=UTF-8' : '')
  if (urlOrigin(url) === PAGE_ORIGIN) return send(url, method, headers, data)
  if (mode === 'same-origin') throw new TypeError('Failed to fetch')

  const unsafeHeaders = [...headers.values.keys()].filter((name) => !SAFELISTED_REQUEST_HEADERS.includes(name) ||
    (name === 'content-type' && headers.get(name) && !SIMPLE_CONTENT_TYPES.includes(headers.get(name).split(';')[0].trim().toLowerCase())))
  const simple = SIMPLE_METHODS.includes(method) && !unsafeHeaders.length
  if (mode === 'no-cors') {
    if (!simple) throw new TypeError(`Failed to execute 'fetch' on 'Window': '${method}' is unsupported in no-cors mode.`)
    await send(url, method, headers, data)
    return new Response(url, () => '', () => new ArrayBuffer(0), { status: 0, statusText: '', type: 'opaque' })
  }
  headers.set('origin', PAGE_ORIGIN)
  if (!simple) {
    const preflightHeaders = new Headers({ origin: PAGE_ORIGIN, 'access-control-request-method': method })
    if (unsafeHeaders.length) preflightHeaders.set('access-control-request-headers', unsafeHeaders.sort().join(','))
    const preflight = await send(url, 'OPTIONS', preflightHeaders, null)
    const reason = !preflight.ok ? "Response to preflight request doesn't pass access control check: It does not have HTTP ok status."
      : corsError(preflight) ||
        (!SIMPLE_METHODS.includes(method) && !(preflight.headers.get('access-control-allow-methods') || '').split(',')
          .map((item) => item.trim()).some((item) => item === '*' || item === method)
          ? `Method ${method} is not allowed by Access-Control-Allow-Methods in preflight response.` : null) ||
        unsafeHeaders.filter((name) => !listed(preflight.headers.get('access-control-allow-headers'), name))
          .map((name) => `Request header field ${name} is not allowed by Access-Control-Allow-Headers in preflight response.`)[0]
    if (reason) throw blocked(url, reason)
  }
  const response = await send(url, method, headers, data)
  const reason = corsError(response)
  if (reason) throw blocked(url, reason)
  response.type = 'cors'
  return exposeHeaders(response)
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
  return request(url, method, options.headers || input?.headers, options.body, options.mode || input?.mode || 'cors')
}
window.Headers = Headers
window.Response = Response

export class XMLHttpRequest extends EventTarget {
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
    if (typeof this[`on${type}`] === 'function') this[`on${type}`](event)
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
