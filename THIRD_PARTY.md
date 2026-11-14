# Third-party code

hydra-native contains or downloads the code below.
CMakeLists.txt downloads the C libraries into `vendor/` and checks their SHA-256 hashes.
pnpm installs the JavaScript packages into `node_modules/`, and esbuild bundles them into `build/hydra-bundle.js`.

Because hydra-native bundles hydra-synth and code of the hydra web editor (AGPL-3.0),
a distributed hydra-native binary with its bundle falls under the AGPL-3.0.

## C libraries (downloaded by `make`)

- QuickJS 2026-06-04 by Fabrice Bellard and Charlie Gordon, MIT license.
  JavaScript engine. https://bellard.org/quickjs/
- stb_truetype.h 1.26 and stb_image_write.h 1.16 by Sean Barrett, public domain or MIT license,
  commit 4e9b368bfc071e97206ea5c6adba68dd0cbf8897 of https://github.com/nothings/stb.
  Glyph atlas of the code overlay (src/overlay.c), the text sources (src/text.c) and PNG output (src/main.c).
- stb_image.h 2.30 by Sean Barrett, public domain or MIT license, same commit. Thumbnails (src/thumbnails.c).
- libremidi 5.4.3 by Jean-Michaël Celerier, BSD-2-Clause license (with parts of RtMidi, MIT license).
  https://github.com/celtera/libremidi. MIDI input and output on macOS (CoreMIDI) and Windows (WinMM) in its
  header-only mode; src/midi_libremidi.cc.
- ANGLE, commit 9684da72c67be785ed049e4c5704964b5d6e1bcb, BSD-3-Clause license. https://angleproject.org
  OpenGL ES on macOS and Windows, as a prebuilt archive of https://github.com/jeremyfa/build-angle.
  The Windows archive of ANGLE also holds d3dcompiler_47.dll of Microsoft, which the Windows zip contains.
- SDL 3, zlib license. https://libsdl.org. The macOS app contains the SDL3 library of Homebrew, and the Windows
  zip the SDL3.dll of the MinGW package of SDL 3.4.2.
- C++/WinRT 2.0.250303.1 by Microsoft, MIT license. https://github.com/microsoft/cppwinrt. The NuGet package
  Microsoft.Windows.CppWinRT gives cppwinrt.exe, which writes the headers of the Windows screen capture
  (src/capture_windows.cpp).
- The runtime of llvm-mingw 20261104 (https://github.com/mstorsjo/llvm-mingw), linked statically into the
  Windows programs: libc++ and libunwind of LLVM (Apache-2.0 with LLVM exception), and the MinGW-w64 runtime
  and winpthreads (MIT and ZPL-2.1 licenses).

hydra-native-patch-bay (src/patch_bay_main.c) links these libraries statically:

- libdatachannel 0.24.6 by Paul-Louis Ageneau, MPL-2.0. WebRTC and the WebSocket of the signalling.
  https://github.com/paullouisageneau/libdatachannel
- libjuice 1.7.4, commit b89c792e3612faf2f12cf35bcc56857313a06be3, by Paul-Louis Ageneau, MPL-2.0. ICE.
  https://github.com/paullouisageneau/libjuice
- usrsctp, commit fec583d54493f879d2ae44a743423bf8a04371ab of the fork of libdatachannel, BSD-3-Clause.
  SCTP for the data channel. https://github.com/paullouisageneau/usrsctp
- libsrtp 2.8.0, commit 24b3bf8f19b6f5ab4cd2bcceb4f4064efca86fd5, by Cisco Systems, BSD-3-Clause. SRTP.
  https://github.com/cisco/libsrtp
- plog, commit 94899e0b926ac1b0f4750bfbd495167b4a6ae9ef, by Sergey Podobry, MIT license. The log of
  libdatachannel. https://github.com/SergiusTheBest/plog
- Mbed TLS 3.6.7, Apache-2.0 or GPL-2.0-or-later (hydra-native uses it under the Apache-2.0). TLS, DTLS and
  DTLS-SRTP. https://github.com/Mbed-TLS/mbedtls
- cJSON 1.7.19 by Dave Gamble and the cJSON contributors, MIT license. The JSON of the signalling and of the
  JSON lines. https://github.com/DaveGamble/cJSON

The libjuice, usrsctp, libsrtp and plog commits are the submodules of the libdatachannel release.

## JavaScript packages (installed by pnpm, bundled by esbuild)

- hydra-synth, commit 9d29a9f4fd8f9081b9759943f38db36f05b9a88f of the main branch (1.4.0 and the
  fixes #201, #202, #203 and #205), AGPL-3.0. https://github.com/hydra-synth/hydra-synth
  The renderer and the API; it runs unchanged. js/regl-shim.js, js/raf-loop-shim.js and js/meyda-shim.js
  replace its dependencies regl, raf-loop and meyda.
- esbuild 0.28.2, MIT license. Build tool only, not in the output.
- socket.io 2.5.0, MIT license. Only for test/signalling_server.cjs.

## Code of the hydra web editor

From https://github.com/hydra-synth/hydra (the editor of hydra.ojack.xyz), commit 680273b, AGPL-3.0:

- js/hydra-editor/Mutator.js (src/views/editor/randomizer/Mutator.js), rewritten to scan the text instead of
  parsing it; see its header.
- js/hydra-editor/examples.json (src/stores/examples.json), the examples of the "random example" button.
- js/hydra-editor/text-elements.js (src/stores/text-elements.js), the English texts of the info panel, unchanged.
- src/lib/patch-bay/rtc-patch-bay.js and pb-live.js, the webRTC patch bay: src/patch_bay_main.c does the same
  in C, with the same messages, roles of the peers and tables of names.
- The confirmation text of the gallery share prompt in src/ui.c (src/stores/gallery-store.js).

From https://github.com/hydra-synth/hydra-server, backend/server.js, AGPL-3.0: the socket.io handlers of the
signalling in test/signalling_server.cjs, without the TURN tokens of Twilio.

From hydra-synth pull request #169 (closed, by the author of hydra-native), types/index.d.ts at commit
40a8de992bc31da496e024a3f5dd137f4d2fba7d: the signatures of out, render and setResolution in
js/api-signatures.js, for the signature help. The pull request has no license statement of its own; the
repository of hydra-synth is under the AGPL-3.0.
https://github.com/hydra-synth/hydra-synth/pull/169

Translations of the info panel come at run time from https://github.com/hydra-synth/l10n.

From hyper-hydra by geikha (https://github.com/geikha/hyper-hydra, hydra-text.js), GPL-3.0: the interface, the
layout rules and the function srcRelMask of the text sources in js/text.js.

## Algorithms from other projects

- src/audio.c computes the "loudness" feature as Meyda 5.6.3 (MIT) does, so that `a.setScale` and `a.setCutoff`
  keep their meaning. No Meyda code is included.

## Fonts

The code overlay and the text sources use the fonts of the system. hydra-native contains no font.

## Algorithms from specifications

- src/editor.c ports findPosH (the motion by groups) and findPosSubword of CodeMirror 5.65.9 (MIT license,
  https://codemirror.net/5/), so that Ctrl+Left, Ctrl+Right and the subword keys stop as in the web editor.

- src/unicode_tables.h comes from the Unicode Character Database 16.0.0 (GraphemeBreakProperty.txt,
  emoji-data.txt, EastAsianWidth.txt; Unicode License v3) and the extendingChars class of CodeMirror 5.65.9 (MIT),
  through tools/unicode_tables.py. src/unicode.h follows the grapheme cluster rules of UAX #29.

The texts of these two licenses are in licenses/ (unicode.txt, codemirror.txt), next to the texts of ANGLE and
SDL3, which the macOS and Windows archives contain as libraries. tools/package.sh copies them into each archive.
