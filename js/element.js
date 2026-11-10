// A small DOM element for the shims: enough of the node, attribute, class and style API for hydra-synth,
// its extensions and p5.js. Nothing is shown; the 2D canvas subclass in js/canvas.js draws for real.

// addEventListener and dispatchEvent for elements and XMLHttpRequest; a handler property such as `onload` runs too.
export class EventTarget {
  constructor () { this.listeners = new Map() }

  addEventListener (type, listener) {
    if (!this.listeners.has(type)) this.listeners.set(type, [])
    this.listeners.get(type).push(listener)
  }

  removeEventListener (type, listener) {
    this.listeners.set(type, (this.listeners.get(type) || []).filter((item) => item !== listener))
  }

  dispatchEvent (event) {
    for (const listener of this.listeners.get(event.type) || []) listener.call(this, event)
    return true
  }
}

const MARKUP_TOKEN = /<!--[\s\S]*?-->|<\/([\w:-]+)\s*>|<([\w:-]+)((?:\s+[\w:.-]+(?:\s*=\s*(?:"[^"]*"|'[^']*'|[^\s>]+))?)*)\s*(\/?)>|[^<]+/g
const MARKUP_ATTRIBUTE = /([\w:.-]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))?/g

// Elements of an HTML or SVG fragment (innerHTML, insertAdjacentHTML), for SVG <filter> definitions; text is
// left out.
function parseMarkup (markup) {
  const root = new Element('template'), stack = [root]
  for (const [token, closing, name, attributes, selfClosing] of String(markup).matchAll(MARKUP_TOKEN)) {
    if (closing) {
      while (stack.length > 1 && stack.pop().localName !== closing) {}
    } else if (name) {
      const element = new Element(name)
      for (const [, key, double, single, bare] of attributes.matchAll(MARKUP_ATTRIBUTE)) {
        element.setAttribute(key, double ?? single ?? bare ?? '')
      }
      stack[stack.length - 1].appendChild(element)
      if (!selfClosing) stack.push(element)
    } else if (token.startsWith('<!--')) {
      continue
    }
  }
  return root.children.slice()
}

export class Element extends EventTarget {
  constructor (tagName) {
    super()
    this.localName = String(tagName) // keeps the case, as SVG names such as feGaussianBlur need it
    this.tagName = String(tagName).toUpperCase()
    this.nodeName = this.tagName
    this.nodeType = 1
    this.style = {}
    this.dataset = {}
    this.attributes = {}
    this.children = []
    this.childNodes = this.children
    this.parentNode = null
    this.parentElement = null
    this.id = ''
    this.className = ''
    const element = this
    this.classList = {
      add: (...names) => { for (const name of names) if (!element.classList.contains(name)) element.className = `${element.className} ${name}`.trim() },
      remove: (...names) => { element.className = element.className.split(/\s+/).filter((item) => !names.includes(item)).join(' ') },
      contains: (name) => element.className.split(/\s+/).includes(name),
      toggle: (name) => element.classList.contains(name) ? (element.classList.remove(name), false) : (element.classList.add(name), true)
    }
  }

  appendChild (child) {
    if (child.parentNode) child.parentNode.removeChild(child)
    child.parentNode = child.parentElement = this
    this.children.push(child)
    return child
  }

  insertBefore (child, reference) {
    const index = this.children.indexOf(reference)
    if (index < 0) return this.appendChild(child)
    child.parentNode = child.parentElement = this
    this.children.splice(index, 0, child)
    return child
  }

  removeChild (child) {
    const index = this.children.indexOf(child)
    if (index >= 0) this.children.splice(index, 1)
    child.parentNode = child.parentElement = null
    return child
  }

  remove () { if (this.parentNode) this.parentNode.removeChild(this) }

  get innerHTML () { return '' }
  set innerHTML (markup) {
    for (const child of this.children.slice()) this.removeChild(child)
    for (const child of parseMarkup(markup)) this.appendChild(child)
  }

  insertAdjacentHTML (position, markup) {
    const elements = parseMarkup(markup), where = String(position).toLowerCase()
    if (where === 'beforeend') for (const element of elements) this.appendChild(element)
    else if (where === 'afterbegin') for (const element of elements.reverse()) this.insertBefore(element, this.children[0])
    else if (this.parentNode) {
      const reference = where === 'beforebegin' ? this : this.parentNode.children[this.parentNode.children.indexOf(this) + 1]
      for (const element of elements) this.parentNode.insertBefore(element, reference)
    }
  }
  setAttribute (name, value) { this.attributes[name] = String(value); if (name === 'id' || name === 'class') this[name === 'class' ? 'className' : 'id'] = String(value) }
  getAttribute (name) { return name in this.attributes ? this.attributes[name] : null }
  hasAttribute (name) { return name in this.attributes }
  removeAttribute (name) { delete this.attributes[name] }
  descendants () { return this.children.flatMap((child) => child.descendants ? [child, ...child.descendants()] : []) }
  getElementsByTagName (name) {
    return this.descendants().filter((element) => name === '*' || element.tagName === String(name).toUpperCase())
  }

  getElementsByClassName (name) { return this.descendants().filter((element) => element.classList.contains(name)) }
  getElementById (id) { return this.descendants().find((element) => element.id === id) || null }
  // Only "#id", ".class" and tag names.
  querySelectorAll (selector) {
    if (selector.startsWith('#')) return [this.getElementById(selector.slice(1))].filter(Boolean)
    if (selector.startsWith('.')) return this.getElementsByClassName(selector.slice(1))
    return this.getElementsByTagName(selector)
  }

  querySelector (selector) { return this.querySelectorAll(selector)[0] || null }
  getBoundingClientRect () {
    const width = this.width || 0, height = this.height || 0
    return { x: 0, y: 0, top: 0, left: 0, right: width, bottom: height, width, height }
  }

  focus () {}
  blur () {}
  get offsetWidth () { return this.width || 0 }
  get offsetHeight () { return this.height || 0 }
  get clientWidth () { return this.width || 0 }
  get clientHeight () { return this.height || 0 }
}
