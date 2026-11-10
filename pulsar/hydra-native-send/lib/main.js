'use strict'
// Sends code to the Unix socket of hydra-native (see ../../README.md).
// Line and block follow atom-hydra: a block is the paragraph around the cursor.
const net = require('net')
const path = require('path')
const { CompositeDisposable } = require('atom')

const SOCKET_PATH = path.join(process.env.XDG_RUNTIME_DIR || '/tmp', 'hydra-native.sock')
const FLASH_MS = 200

function send (code) {
  const socket = net.createConnection(SOCKET_PATH, () => socket.end(code))
  let reply = ''
  socket.on('data', (chunk) => { reply += chunk })
  socket.on('end', () => {
    if (reply.trim() !== 'ok') atom.notifications.addError('hydra-native', { detail: reply, dismissable: true })
  })
  socket.on('error', (error) => atom.notifications.addWarning(`hydra-native: ${error.message}`))
}

function flash (editor, range) {
  const marker = editor.markBufferRange(range, { invalidate: 'touch' })
  editor.decorateMarker(marker, { type: 'line', class: 'hydra-flash' })
  setTimeout(() => marker.destroy(), FLASH_MS)
}

function evalLine (editor) {
  const range = editor.getSelectedBufferRange()
  if (!range.isEmpty()) return evalRange(editor, range)
  const row = editor.getCursorBufferPosition().row
  evalRange(editor, editor.getBuffer().rangeForRow(row))
}

function evalBlock (editor) {
  const isBlank = (row) => !/\S/.test(editor.lineTextForBufferRow(row) || '')
  let start = editor.getCursorBufferPosition().row
  let end = start
  while (start > 0 && !isBlank(start - 1)) start--
  while (end < editor.getLastBufferRow() && !isBlank(end + 1)) end++
  evalRange(editor, [[start, 0], [end, editor.lineTextForBufferRow(end).length]])
}

function evalRange (editor, range) {
  flash(editor, range)
  send(editor.getTextInBufferRange(range))
}

let subscriptions

module.exports = {
  activate () {
    subscriptions = new CompositeDisposable()
    subscriptions.add(atom.commands.add('atom-text-editor', {
      'hydra-native:evalLine': () => evalLine(atom.workspace.getActiveTextEditor()),
      'hydra-native:evalBlock': () => evalBlock(atom.workspace.getActiveTextEditor()),
      'hydra-native:evalCode': () => evalRange(atom.workspace.getActiveTextEditor(), atom.workspace.getActiveTextEditor().getBuffer().getRange())
    }))
  },
  deactivate () {
    subscriptions.dispose()
  }
}
