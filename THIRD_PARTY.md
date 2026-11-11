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
- HarfBuzz 14.6.0, "Old MIT" license. https://github.com/harfbuzz/harfbuzz
  Text shaping of panels and prompts (src/overlay.c); built from the amalgamated src/harfbuzz.cc.
- libremidi 5.4.3 by Jean-Michaël Celerier, BSD-2-Clause license (with parts of RtMidi, MIT license).
  https://github.com/celtera/libremidi. MIDI input and output on macOS (CoreMIDI) and Windows (WinMM) in its
  header-only mode; src/midi_libremidi.cc.
- SheenBidi 2.9.0 by Muhammad Tayyab Akram, Apache-2.0 license. https://github.com/Tehreer/SheenBidi
  The Unicode bidi algorithm (UAX #9) for the text of panels and prompts (src/overlay.c); built from
  Source/SheenBidi.c with SB_CONFIG_UNITY.
- ANGLE, commit 9684da72c67be785ed049e4c5704964b5d6e1bcb, BSD-3-Clause license. https://angleproject.org
  OpenGL ES on macOS and Windows, as a prebuilt archive of https://github.com/jeremyfa/build-angle.
- SDL 3, zlib license. https://libsdl.org. The macOS app contains the SDL3 library of Homebrew.

## JavaScript packages (installed by pnpm, bundled by esbuild)

- hydra-synth, commit 9d29a9f4fd8f9081b9759943f38db36f05b9a88f of the main branch (1.4.0 and the
  fixes #201, #202, #203 and #205), AGPL-3.0. https://github.com/hydra-synth/hydra-synth
  The renderer and the API; it runs unchanged. js/regl-shim.js, js/raf-loop-shim.js and js/meyda-shim.js
  replace its dependencies regl, raf-loop and meyda.
- esbuild 0.28.2, MIT license. Build tool only, not in the output.

## JavaScript packages of the patch bay (installed by pnpm, run by Node.js in tools/patch-bay.mjs)

- @roamhq/wrtc 0.8.0 and its prebuilt binary @roamhq/wrtc-linux-x64 0.8.1 (wrtc.node, which contains the
  WebRTC library of Google), BSD-2-Clause license. https://github.com/WonderInventions/node-webrtc
  The WebRTC stack (ICE, DTLS-SRTP, VP8) of pb and s0.initStream.
  On macOS and Windows, pnpm installs the prebuilt binary of that system instead: @roamhq/wrtc-darwin-arm64,
  @roamhq/wrtc-darwin-x64 or @roamhq/wrtc-win32-x64, version 0.8.0, under the same license.
- simple-peer 9.11.1 and socket.io-client 2.5.0 (the versions of the web editor), MIT license.
- socket.io 2.5.0, MIT license. Only for test/signalling_server.cjs.

## Code of the hydra web editor

From https://github.com/hydra-synth/hydra (the editor of hydra.ojack.xyz), commit 680273b, AGPL-3.0:

- js/hydra-editor/Mutator.js (src/views/editor/randomizer/Mutator.js), rewritten to scan the text instead of
  parsing it; see its header.
- js/hydra-editor/examples.json (src/stores/examples.json), the examples of the "random example" button.
- js/hydra-editor/text-elements.js (src/stores/text-elements.js), the English texts of the info panel, unchanged.
- js/hydra-editor/rtc-patch-bay.js and js/hydra-editor/pb-live.js (src/lib/patch-bay/), the webRTC patch bay;
  tools/patch-bay.mjs runs them in Node.js. Changed: `inherits` comes from node:util, and the default name
  comes from crypto.randomUUID() instead of shortid.
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

The code overlay and the text sources use the fonts of the system.
`make` downloads one font: Noto Emoji (monochrome), SIL Open Font License 1.1, from
https://github.com/google/fonts (ofl/notoemoji, commit 8b0a1d0f5983c89bc2b93f1b5fb55f9e252744b5), into
vendor/fonts with its OFL.txt. It draws emoji in the overlay when the system has no color emoji font.

## Algorithms from specifications

- src/bidi.c ports bidiOrdering, and src/editor.c ports moveVisually, getBidiPartAt and endOfLine of
  CodeMirror 5.65.9 (MIT license, https://codemirror.net/5/), so that the arrow keys move in right-to-left
  code as in the web editor.

- src/unicode_tables.h comes from the Unicode Character Database 16.0.0 (GraphemeBreakProperty.txt,
  emoji-data.txt, EastAsianWidth.txt; Unicode License v3) and the extendingChars class of CodeMirror 5.65.9 (MIT),
  through tools/unicode_tables.py. src/unicode.h follows the grapheme cluster rules of UAX #29.
