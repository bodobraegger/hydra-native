// The part of the EventEmitter of Node.js that `pb` offers, as in the web editor: on, once, off, emit and
// removeAllListeners, with their aliases.
export class EventEmitter {
  #listeners = new Map()

  on (type, listener) {
    if (!this.#listeners.has(type)) this.#listeners.set(type, [])
    this.#listeners.get(type).push(listener)
    return this
  }

  once (type, listener) {
    const wrapper = (...args) => {
      this.off(type, wrapper)
      listener.apply(this, args)
    }
    wrapper.listener = listener
    return this.on(type, wrapper)
  }

  off (type, listener) {
    const listeners = this.#listeners.get(type) || []
    const index = listeners.findIndex((item) => item === listener || item.listener === listener)
    if (index >= 0) listeners.splice(index, 1)
    return this
  }

  emit (type, ...args) {
    const listeners = [...(this.#listeners.get(type) || [])]
    for (const listener of listeners) listener.apply(this, args)
    return listeners.length > 0
  }

  removeAllListeners (type) {
    if (type === undefined) this.#listeners.clear()
    else this.#listeners.delete(type)
    return this
  }

  listeners (type) { return (this.#listeners.get(type) || []).map((item) => item.listener || item) }
}

EventEmitter.prototype.addListener = EventEmitter.prototype.on
EventEmitter.prototype.removeListener = EventEmitter.prototype.off
