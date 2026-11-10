// A small DOM element for the shims: enough of the node, attribute, class and style API for hydra-synth,
// its extensions and p5.js. Nothing is shown; the 2D canvas subclass in js/canvas.js draws for real.

export class Element {
  constructor (tagName) {
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
    this.listeners = new Map()
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
  setAttribute (name, value) { this.attributes[name] = String(value); if (name === 'id' || name === 'class') this[name === 'class' ? 'className' : 'id'] = String(value) }
  getAttribute (name) { return name in this.attributes ? this.attributes[name] : null }
  hasAttribute (name) { return name in this.attributes }
  removeAttribute (name) { delete this.attributes[name] }
  addEventListener (type, listener) {
    if (!this.listeners.has(type)) this.listeners.set(type, [])
    this.listeners.get(type).push(listener)
  }

  removeEventListener (type, listener) {
    this.listeners.set(type, (this.listeners.get(type) || []).filter((item) => item !== listener))
  }

  dispatchEvent (event) {
    for (const listener of this.listeners.get(event.type) || []) listener(event)
    return true
  }

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
