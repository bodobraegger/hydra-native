# Third-party code

hydra-native contains or downloads the code below.
CMakeLists.txt downloads the C libraries into `vendor/` and checks their SHA-256 hashes; pnpm installs the JavaScript packages into `node_modules/`.
esbuild bundles the JavaScript packages into `build/hydra-bundle.js`.

Because hydra-native bundles hydra-synth and code of the hydra web editor (AGPL-3.0),
a distributed hydra-native binary with its bundle falls under the AGPL-3.0.

## C libraries (downloaded by `make`)

- QuickJS 2026-06-04 by Fabrice Bellard and Charlie Gordon, MIT license.
  JavaScript engine. https://bellard.org/quickjs/
- stb_truetype.h 1.26 and stb_image_write.h 1.16 by Sean Barrett, public domain or MIT license,
  commit 4e9b368bfc071e97206ea5c6adba68dd0cbf8897 of https://github.com/nothings/stb.
  Glyph atlas of the code overlay (src/overlay.c) and PNG output (src/main.c).
- stb_image.h 2.30 by Sean Barrett, public domain or MIT license, same commit. Thumbnails (src/thumbnails.c).
- HarfBuzz 14.6.0, "Old MIT" license. https://github.com/harfbuzz/harfbuzz
  Text shaping of panels and prompts (src/overlay.c); built from the amalgamated src/harfbuzz.cc.
- webrtc-audio-processing 1.3 (freedesktop.org, from the WebRTC code of Chrome), BSD-3-Clause license.
  https://gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing
  On Linux, the system library libwebrtc-audio-processing-1.so.3 (installed with PipeWire, not downloaded)
  processes the microphone, and `make` downloads the release tarball only for its headers. On macOS and Windows,
  cmake/webrtc-audio-processing.cmake builds the sources of the tarball. src/webrtc_apm.cc links the library into
  build/libhydra-webrtc-apm, which src/audio.c opens at run time.
- Abseil 20230125.1 (the version of the abseil-cpp.wrap of webrtc-audio-processing 1.3), Apache-2.0 license.
  https://github.com/abseil/abseil-cpp. Headers only, for webrtc-audio-processing.
- libremidi 5.4.3 by Jean-Michaël Celerier, BSD-2-Clause license (with parts of RtMidi, MIT license).
  https://github.com/celtera/libremidi. MIDI input and output on macOS (CoreMIDI) and Windows (WinMM) in its
  header-only mode; src/midi_libremidi.cc.
- miniaudio 0.11.25 by David Reid, public domain or MIT No Attribution (MIT-0), miniaudio.h and miniaudio.c of
  commit 9634bedb5b5a2ca38c1ee7108a9358a4e233f14d of https://github.com/mackron/miniaudio.
  The audio input on macOS (Core Audio) and Windows (WASAPI, also the loopback of the output);
  src/audio_input_miniaudio.c.
- SheenBidi 2.9.0 by Muhammad Tayyab Akram, Apache-2.0 license. https://github.com/Tehreer/SheenBidi
  The Unicode bidi algorithm (UAX #9) for the text of panels and prompts (src/overlay.c); built from
  Source/SheenBidi.c with SB_CONFIG_UNITY.
- plutovg 1.3.3 by Samuel Ugochukwu, MIT license. https://github.com/sammycage/plutovg
  2D canvas (src/canvas2d.c). Its files plutovg-ft-*.c come from FreeType and are under the FreeType License (FTL);
  plutovg contains its own copy of stb_truetype.

## JavaScript packages (installed by pnpm, bundled by esbuild)

- hydra-synth, commit 9d29a9f4fd8f9081b9759943f38db36f05b9a88f of the main branch (1.4.0 and the
  fixes #201, #202, #203 and #205), AGPL-3.0. https://github.com/hydra-synth/hydra-synth
  The renderer and the API; it runs unchanged. js/regl-shim.js, js/raf-loop-shim.js and js/meyda-shim.js
  replace its dependencies regl, raf-loop and meyda.
- acorn 8.19.0, astring 1.9.0 and astravel 0.5.0, MIT license. The parser and code generator of the mutator.
- p5.js 1.6.0 (the version of public/p5.min.js in the web editor), LGPL-2.1. `make` copies lib/p5.min.js of
  the npm package to build/p5.min.js; js/p5.js loads it on the first use of P5.
- esbuild 0.28.2, MIT license. Build tool only, not in the output.

## JavaScript packages of the patch bay (installed by pnpm, run by Node.js in tools/patch-bay.mjs)

- @roamhq/wrtc 0.8.0 and its prebuilt binary @roamhq/wrtc-linux-x64 0.8.1 (wrtc.node, which contains the
  WebRTC library of Google), BSD-2-Clause license. https://github.com/WonderInventions/node-webrtc
  The WebRTC stack (ICE, DTLS-SRTP, VP8) of pb and s0.initStream. The user allowed this prebuilt download.
  On macOS and Windows, pnpm installs the prebuilt binary of that system instead: @roamhq/wrtc-darwin-arm64,
  @roamhq/wrtc-darwin-x64 or @roamhq/wrtc-win32-x64, version 0.8.0, under the same license.
- simple-peer 9.11.1 and socket.io-client 2.5.0 (the versions of the web editor), MIT license; shortid 2.2.17,
  MIT license; inherits 2.0.4, ISC license; events 3.3.0, MIT license (also for js/patch-bay.js in the bundle).
- socket.io 2.5.0, MIT license. Only for test/signalling_server.cjs.

## Code of the hydra web editor

From https://github.com/hydra-synth/hydra (the editor of hydra.ojack.xyz), commit 680273b, AGPL-3.0:

- js/hydra-editor/Mutator.js (src/views/editor/randomizer/Mutator.js), with changed imports and two fixes
  that the file header lists.
- js/hydra-editor/UndoStack.js (src/views/editor/randomizer/UndoStack.js), unchanged.
- js/hydra-editor/examples.json (src/stores/examples.json), the examples of the "random example" button.
- js/hydra-editor/text-elements.js (src/stores/text-elements.js), the English texts of the info panel, unchanged.
- js/hydra-editor/rtc-patch-bay.js and js/hydra-editor/pb-live.js (src/lib/patch-bay/), the webRTC patch bay,
  unchanged; tools/patch-bay.mjs runs them in Node.js.

From https://github.com/hydra-synth/hydra-server, backend/server.js, AGPL-3.0: the socket.io handlers of the
signalling in test/signalling_server.cjs, without the TURN tokens of Twilio.
- The class body of P5 in js/p5.js (src/lib/p5-wrapper.js).
- The confirmation text of the gallery share prompt in src/ui.c (src/stores/gallery-store.js).

From hydra-synth pull request #169 (closed, by the author of hydra-native), types/index.d.ts at commit
40a8de992bc31da496e024a3f5dd137f4d2fba7d: the signatures of out, render and setResolution in
js/api-signatures.js, for the signature help. The pull request has no license statement of its own; the
repository of hydra-synth is under the AGPL-3.0.
https://github.com/hydra-synth/hydra-synth/pull/169

Translations of the info panel come at run time from https://github.com/hydra-synth/l10n.

## Algorithms from other projects

- src/audio.c computes the "loudness" feature as Meyda 5.6.3 (MIT) does, so that `a.setScale` and `a.setCutoff`
  keep their meaning. No Meyda code is included.

## Fonts

The code overlay and the 2D canvas use fonts that are installed on the system (default: DejaVu Sans Mono).
`make` downloads one font: Noto Emoji (monochrome), SIL Open Font License 1.1, from
https://github.com/google/fonts (ofl/notoemoji, commit 8b0a1d0f5983c89bc2b93f1b5fb55f9e252744b5), into
vendor/fonts with its OFL.txt. It draws emoji in the overlay when the system has no color emoji font.
The color emoji come from the font that fontconfig gives for `emoji:color=true`, usually Noto Color Emoji of
the system.

## Data

- js/webgl-constants.js holds the constant values of GLES2/gl2.h of the Khronos Group (generated from the
  system header) plus the WebGL constants.

## Algorithms from specifications

- src/bidi.c ports bidiOrdering, and src/editor.c ports moveVisually, getBidiPartAt and endOfLine of
  CodeMirror 5.65.9 (MIT license, https://codemirror.net/5/), so that the arrow keys move in right-to-left
  code as in the web editor.

- src/unicode_tables.h comes from the Unicode Character Database 16.0.0 (GraphemeBreakProperty.txt,
  emoji-data.txt, EastAsianWidth.txt; Unicode License v3) and the extendingChars class of CodeMirror 5.65.9 (MIT),
  through tools/unicode_tables.py. src/unicode.h follows the grapheme cluster rules of UAX #29.

- src/svg_filter.c ports the feTurbulence reference code and uses the box blur sizes of the Filter Effects
  Module Level 1 specification of the W3C (https://www.w3.org/TR/filter-effects-1/).
