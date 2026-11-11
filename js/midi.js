// navigator.requestMIDIAccess() for MIDI input and output, through src/midi.h.
import { onAfterFrame } from './dom-shim.js'
import { EventTarget } from './element.js'

class MIDIInput extends EventTarget {
  constructor (id, name, manufacturer) {
    super()
    Object.assign(this, { id, name, manufacturer, type: 'input', state: 'connected', connection: 'open' })
  }

  open () { return Promise.resolve(this) }
  close () { return Promise.resolve(this) }

  receive (bytes, timeStamp) {
    const data = Uint8Array.from(bytes)
    this.dispatchEvent({ data, timeStamp, target: this, currentTarget: this, type: 'midimessage' })
  }
}

class MIDIOutput {
  constructor (id, name, manufacturer) {
    Object.assign(this, { id, name, manufacturer, type: 'output', state: 'connected', connection: 'open' })
    this.pending = []
  }

  // A timestamp in the future (performance.now() time) waits until then, with the resolution of a frame.
  send (data, timestamp) {
    const bytes = Array.from(data)
    const delay = timestamp ? timestamp - performance.now() : 0
    if (delay > 0) {
      const timer = setTimeout(() => {
        this.pending = this.pending.filter((item) => item !== timer)
        native.midiSend(this.id, bytes)
      }, delay)
      this.pending.push(timer)
    } else {
      native.midiSend(this.id, bytes)
    }
  }

  clear () {
    for (const timer of this.pending) clearTimeout(timer)
    this.pending = []
  }

  addEventListener () {}
  removeEventListener () {}
  open () { return Promise.resolve(this) }
  close () { return Promise.resolve(this) }
}

let access

function updatePorts (map, current, Port) {
  const ids = new Set(current.map(([id]) => id))
  for (const id of [...map.keys()]) if (!ids.has(id)) map.delete(id)
  for (const [id, name, manufacturer] of current) if (!map.has(id)) map.set(id, new Port(id, name, manufacturer))
}

function updateInputs () {
  updatePorts(access.inputs, native.midiInputs(), MIDIInput)
  updatePorts(access.outputs, native.midiOutputs(), MIDIOutput)
  if (access.onstatechange) access.onstatechange({ target: access })
}

navigator.requestMIDIAccess = () => {
  if (access) return Promise.resolve(access)
  if (!native.midiStart()) return Promise.reject(new Error('the MIDI system is not available'))
  access = { inputs: new Map(), outputs: new Map(), sysexEnabled: false, onstatechange: null }
  updateInputs()
  onAfterFrame(() => {
    const { changed, messages } = native.midiPoll()
    if (changed) updateInputs()
    for (const [id, time, ...bytes] of messages) {
      const input = access.inputs.get(id)
      if (input) input.receive(bytes, time)
    }
  })
  return Promise.resolve(access)
}
