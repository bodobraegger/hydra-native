// navigator.requestMIDIAccess() for MIDI input, through the ALSA sequencer of src/midi.c.
// MIDI output is not available (the outputs map stays empty).
import { onAfterFrame } from './dom-shim.js'

class MIDIInput {
  constructor (id, name, manufacturer) {
    Object.assign(this, { id, name, manufacturer, type: 'input', state: 'connected', connection: 'open' })
    this.onmidimessage = null
    this.listeners = []
  }

  addEventListener (type, listener) { if (type === 'midimessage') this.listeners.push(listener) }
  removeEventListener (type, listener) { this.listeners = this.listeners.filter((item) => item !== listener) }
  open () { return Promise.resolve(this) }
  close () { return Promise.resolve(this) }

  receive (bytes, timeStamp) {
    const event = { data: Uint8Array.from(bytes), timeStamp, target: this, currentTarget: this, type: 'midimessage' }
    if (this.onmidimessage) this.onmidimessage(event)
    for (const listener of this.listeners) listener(event)
  }
}

let access

function updateInputs () {
  const current = native.midiInputs()
  const ids = new Set(current.map(([id]) => id))
  for (const id of [...access.inputs.keys()]) if (!ids.has(id)) access.inputs.delete(id)
  for (const [id, name, manufacturer] of current) {
    if (!access.inputs.has(id)) access.inputs.set(id, new MIDIInput(id, name, manufacturer))
  }
  if (access.onstatechange) access.onstatechange({ target: access })
}

navigator.requestMIDIAccess = () => {
  if (access) return Promise.resolve(access)
  if (!native.midiStart()) return Promise.reject(new Error('the ALSA sequencer is not available'))
  access = { inputs: new Map(), outputs: new Map(), sysexEnabled: false, onstatechange: null }
  updateInputs()
  onAfterFrame((now) => {
    const { changed, messages } = native.midiPoll()
    if (changed) updateInputs()
    for (const [id, ...bytes] of messages) {
      const input = access.inputs.get(id)
      if (input) input.receive(bytes, now)
    }
  })
  return Promise.resolve(access)
}
