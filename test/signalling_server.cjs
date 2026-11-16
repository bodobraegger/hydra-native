// A local copy of the signalling of hydra-server (https://github.com/hydra-synth/hydra-server, backend/server.js,
// AGPL-3.0, by the hydra authors) for the tests of the patch bay: the socket.io 2 handlers unchanged, without the
// gallery and the static files. In place of the iceServers of a Twilio token, 'ready' sends those of the environment
// variable ICE_SERVERS (JSON), and with LOG_CANDIDATES each candidate of a peer goes to stdout. It listens on
// 127.0.0.1 only.
// usage: node test/signalling_server.cjs [port]; prints "listening PORT".
const http = require('http')

const server = http.createServer((request, response) => {
  response.writeHead(404)
  response.end()
})
const io = require('socket.io')(server, { cors: { origin: true, credentials: true } })

io.on('connection', function (socket) {
  var thisRoom = null
  socket.on('join', function (room, _userData) {
    thisRoom = room

    var peers = io.nsps['/'].adapter.rooms[room] ? Object.keys(io.nsps['/'].adapter.rooms[room].sockets) : []

    socket.emit('ready', {
      id: socket.id,
      peers: peers,
      servers: process.env.ICE_SERVERS ? JSON.parse(process.env.ICE_SERVERS) : undefined
    })

    socket.join(thisRoom)
    socket.to(thisRoom).emit('new peer', socket.id)
    socket.emit("peers", peers);
  })

  socket.on('broadcast', function (data) {
    socket.to(thisRoom).emit('broadcast', data)
  })

  // pass message from one peer to another
  socket.on('message', function (data) {
    const candidate = data.message && data.message.candidate
    if (process.env.LOG_CANDIDATES && candidate) console.log('candidate ' + candidate.candidate)
    var client = io.sockets.connected[data.id];

    client && client.emit('message', {
      id: socket.id,
      label: socket.label,
      type: data.type,
      message: data.message
    });
  })
})

server.listen(Number(process.argv[2] || 0), '127.0.0.1', () => console.log(`listening ${server.address().port}`))
