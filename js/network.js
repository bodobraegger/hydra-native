import { EventTarget } from './element.js'

// fetch() and XMLHttpRequest for sketches and libraries (p5.js loadFont, loadJSON): http(s) URLs download with
// curl (src/fetch.c), other URLs are files relative to the working directory. Only GET works, and a failed
// download is a network error, also for an HTTP error status.

const URL_PATTERN = /^https?:\/\//
const STATUS_OK = 200
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
  constructor () { this.values = new Map() }
  get (name) { return this.values.get(String(name).toLowerCase()) ?? null }
  has (name) { return this.values.has(String(name).toLowerCase()) }
  forEach () {}
}

class Response {
  constructor (url, readText, readBuffer) {
    Object.assign(this, { url, ok: true, status: STATUS_OK, statusText: 'OK', type: 'basic', redirected: false })
    this.headers = new Headers()
    this.readText = readText
    this.readBuffer = readBuffer
  }

  text () { return Promise.resolve().then(this.readText) }
  json () { return this.text().then(JSON.parse) }
  arrayBuffer () { return Promise.resolve().then(this.readBuffer) }
}

function download (url) {
  return new Promise((resolve, reject) => {
    if (!URL_PATTERN.test(url)) {
      const path = url.replace(/^file:\/\//, '')
      try {
        native.readFile(path, true) // fails here for a missing file, as a network error
      } catch (error) {
        return reject(new TypeError(`Failed to fetch ${url}: ${error.message}`))
      }
      return resolve(new Response(url, () => native.readFile(path), () => native.readFile(path, true)))
    }
    pendingFetches.set(native.fetchStart(url), (ok, text, buffer) => {
      if (ok) resolve(new Response(url, () => text, () => buffer.slice(0)))
      else reject(new TypeError(`Failed to fetch ${url}`))
    })
  })
}

window.fetch = (input, options = {}) => {
  const method = String(options.method || input?.method || 'GET').toUpperCase()
  if (method !== 'GET') return Promise.reject(new TypeError(`fetch: hydra-native sends only GET requests, not ${method}`))
  return download(String(input?.url ?? input))
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
    this.change(READY_STATES.OPENED)
  }

  setRequestHeader () {}
  overrideMimeType () {}
  getResponseHeader () { return null }
  getAllResponseHeaders () { return '' }
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

  send () {
    window.fetch(this.url, { method: this.method }).then(async (response) => {
      if (this.aborted) return
      const binary = this.responseType === 'arraybuffer' || this.responseType === 'blob'
      const body = binary ? await response.arrayBuffer() : await response.text()
      Object.assign(this, { status: response.status, statusText: response.statusText, responseURL: this.url })
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
