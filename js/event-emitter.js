// The part of the EventEmitter of Node.js that `pb` needs, as in the web editor.
export class EventEmitter {
  #listeners = new Map()

  on (type, listener) {
    if (!this.#listeners.has(type)) this.#listeners.set(type, [])
    this.#listeners.get(type).push(listener)
    return this
  }

  off (type, listener) {
    const listeners = this.#listeners.get(type) || []
    const index = listeners.indexOf(listener)
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
}
