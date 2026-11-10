# Third-party code

hydra-native contains or downloads the code below.
The Makefile downloads the C libraries into `vendor/`; pnpm installs the JavaScript packages into `node_modules/`.
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
- SheenBidi 2.9.0 by Muhammad Tayyab Akram, Apache-2.0 license. https://github.com/Tehreer/SheenBidi
  The Unicode bidi algorithm (UAX #9) for the text of panels and prompts (src/overlay.c); built from
  Source/SheenBidi.c with SB_CONFIG_UNITY.
- plutovg 1.3.3 by Samuel Ugochukwu, MIT license. https://github.com/sammycage/plutovg
  2D canvas (src/canvas2d.c). Its files plutovg-ft-*.c come from FreeType and are under the FreeType License (FTL);
  plutovg contains its own copy of stb_truetype.

## JavaScript packages (installed by pnpm, bundled by esbuild)

- hydra-synth 1.4.0, AGPL-3.0. https://github.com/hydra-synth/hydra-synth
  The renderer and the API; it runs unchanged. js/regl-shim.js, js/raf-loop-shim.js and js/meyda-shim.js
  replace its dependencies regl, raf-loop and meyda.
- js-beautify 1.15.4, MIT license. The formatter of the web editor (Shift+Ctrl+F).
- acorn 8.19.0, astring 1.9.0 and astravel 0.5.0, MIT license. The parser and code generator of the mutator.
- p5.js 1.6.0 (the version of public/p5.min.js in the web editor), LGPL-2.1. `make` copies lib/p5.min.js of
  the npm package to build/p5.min.js; js/p5.js loads it on the first use of P5.
- esbuild 0.28.2, MIT license. Build tool only, not in the output.

## Code of the hydra web editor

From https://github.com/hydra-synth/hydra (the editor of hydra.ojack.xyz), commit 680273b, AGPL-3.0:

- js/hydra-editor/Mutator.js (src/views/editor/randomizer/Mutator.js), with changed imports and two fixes
  that the file header lists.
- js/hydra-editor/UndoStack.js (src/views/editor/randomizer/UndoStack.js), unchanged.
- js/hydra-editor/examples.json (src/stores/examples.json), the examples of the "random example" button.
- js/hydra-editor/text-elements.js (src/stores/text-elements.js), the English texts of the info panel, unchanged.
- The class body of P5 in js/p5.js (src/lib/p5-wrapper.js).
- The confirmation text of the gallery share prompt in src/ui.c (src/stores/gallery-store.js).

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
