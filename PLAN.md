# Plan

Work for later versions of hydra-native.

## Build without pnpm

Goal: a plain `make` needs no Node.js and no pnpm. Only the patch bay keeps them.

The build gets two things from npm today. CMake can download each of them with a SHA-256 hash, as it does for the
C libraries in vendor/:

- the source of hydra-synth at the commit in package.json (a tarball of GitHub);
- the esbuild binary of the system (the npm tarball of @esbuild/linux-x64, @esbuild/darwin-arm64 or
  @esbuild/win32-x64, in one version).

The bundle then uses `--alias:hydra-synth=vendor/hydra-synth`. The bundle has no other npm package (see
build/hydra-bundle.meta.json).

The patch bay gets its own package.json and lockfile in tools/, with @roamhq/wrtc, simple-peer,
socket.io-client, shortid and inherits, and socket.io for test/signalling_server.cjs. Only the people who use the
patch bay and the release CI run `pnpm install` there. These files move with it:

- js/hydra-editor/rtc-patch-bay.js and js/hydra-editor/pb-live.js, which only Node.js runs (tools/patch-bay.mjs),
  so that their imports resolve from tools/node_modules;
- the esbuild call and the license list of the patch bay in tools/package.sh.

Then remove the root package.json, pnpm-lock.yaml and pnpm-workspace.yaml, and the pnpm steps of the macOS job in
.github/workflows/build.yml. Update README.md ("Build"), THIRD_PARTY.md and the license list of tools/package.sh,
which reads the esbuild metafiles.

## Before the repository becomes public

The AGPL-3.0 requires the complete source for each person who gets the program. Check these points:

- The release archive contains the source of hydra-native (`source/`), but not the source of the C libraries,
  which CMake downloads. A public repository with the pinned URLs and hashes covers this; else attach a source
  archive with vendor/ to each release.
- licenses/ has no text of the Unicode License v3 (src/unicode_tables.h) and of the MIT license of CodeMirror 5
  (code ported in src/bidi.c and src/editor.c). THIRD_PARTY.md names both.
- licenses/node has only the license of the Node.js module @roamhq/wrtc, not the licenses of the WebRTC library
  of Google in wrtc.node.

## Patch bay on the WebRTC of GStreamer

Goal: `pb` and `s0.initStream` work with the web editor as now, without the prebuilt WebRTC module
(@roamhq/wrtc, 27 MB of the 31 MB product), without Node.js and without the npm packages simple-peer and
socket.io-client. The WebRTC stack is `webrtcbin` of GStreamer, a system package like the ones of `initCam`.

A new helper, `hydra-native-patch-bay` (src/patch_bay_main.c, C with GLib), replaces tools/patch-bay.mjs. It
keeps the interface of that file, so that src/patch_bay.c and js/patch-bay.js stay as they are:

- arguments `SERVER ROOM SOCKET`;
- stdin: one JSON object per line, `{ type: 'call', method, args }` for initSource, setName and broadcast;
- stdout: one JSON object per line, `state`, `publish`, `video`, `video closed`, `broadcast`, `message`, `log`;
- the local socket SOCKET for the frames: `output WIDTHxHEIGHT` in, `stream-N-WIDTHxHEIGHT` out, raw RGBA.

The header of tools/patch-bay.mjs describes each message. Read it and the two files of the web editor that it
runs (js/hydra-editor/rtc-patch-bay.js, js/hydra-editor/pb-live.js) before the first line of code: the helper
must send the same messages as they do.

Parts of the helper:

1. Signalling. The server speaks socket.io version 2 (Engine.IO 3) over a WebSocket (libsoup 3):
   `GET /socket.io/?EIO=3&transport=websocket`, the packets `0` open, `2`/`3` ping and pong at the interval
   of the open packet, `40` connect, `42["event", ...]` events. Events: `join` (room, { uuid }), `ready`
   ({ id, peers }), `new peer` (id), `message` ({ id, type: 'signal', message }), `broadcast`.
   test/signalling_server.cjs is the server of the tests.
2. Peers. One `webrtcbin` per peer, with the roles of rtc-patch-bay.js (who is the initiator). The `message`
   of a signal is what simple-peer 9 sends: `{ type: 'offer' | 'answer', sdp }`, `{ candidate: { candidate,
   sdpMLineIndex, sdpMid } }`, and `{ renegotiate: true }` or `{ transceiverRequest }`, which the helper must
   answer as simple-peer does. The data channel of simple-peer carries the JSON of pb-live.js (names).
3. Media. Output: the frames of the socket to `appsrc ! videoconvert ! vp8enc deadline=1 ! rtpvp8pay !
   webrtcbin`, only while a peer asked for the stream (`publish`). Input: `webrtcbin ! rtpvp8depay ! vp8dec !
   videoconvert ! video/x-raw,format=RGBA ! appsink` to a `stream-N-WxH` connection.
4. STUN: the servers that rtc-patch-bay.js gives to simple-peer.

Steps:

1. Build: CMake target `hydra-native-patch-bay` on Linux, with pkg-config modules gstreamer-1.0,
   gstreamer-webrtc-1.0, gstreamer-sdp-1.0, gstreamer-app-1.0, libsoup-3.0 and json-glib-1.0 (Ubuntu:
   libgstreamer1.0-dev, libgstreamer-plugins-base1.0-dev, libgstreamer-plugins-bad1.0-dev, libsoup-3.0-dev,
   libjson-glib-dev). Without them the target is skipped with a message, and `pb` reports that the patch bay
   is not built. Run time: gstreamer1.0-plugins-bad, gstreamer1.0-plugins-good, gstreamer1.0-nice.
2. Signalling alone, against test/signalling_server.cjs: join, ready, peers.
3. Two helpers that exchange a stream: test/patch_bay_test.py must pass unchanged with the new helper.
4. A stream with the web editor at https://hydra.ojack.xyz in both directions, by hand (step for the wizard
   tools/release-tests.sh). This is the step with the most risk: the message formats of simple-peer.
5. Remove tools/patch-bay.mjs, js/hydra-editor/rtc-patch-bay.js, js/hydra-editor/pb-live.js, the packages
   @roamhq/wrtc, simple-peer and socket.io-client, the wrtc copy and license lines of tools/package.sh, and
   `nodejs` from the package lists. socket.io stays as a development package for the test server.
6. macOS: no system GStreamer. The patch bay there needs GStreamer from Homebrew, or stays out of the app;
   decide after step 4.
