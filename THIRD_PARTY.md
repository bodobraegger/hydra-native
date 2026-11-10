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
- plutovg 1.3.3 by Samuel Ugochukwu, MIT license. https://github.com/sammycage/plutovg
  2D canvas (src/canvas2d.c). Its files plutovg-ft-*.c come from FreeType and are under the FreeType License (FTL);
  plutovg contains its own copy of stb_truetype.

## JavaScript packages (installed by pnpm, bundled by esbuild)

- hydra-synth 1.4.0, AGPL-3.0. https://github.com/hydra-synth/hydra-synth
  The renderer and the API; it runs unchanged. js/regl-shim.js, js/raf-loop-shim.js and js/meyda-shim.js
  replace its dependencies regl, raf-loop and meyda.
- js-beautify 1.15.4, MIT license. The formatter of the web editor (Shift+Ctrl+F).
- acorn 8.19.0, astring 1.9.0 and astravel 0.5.0, MIT license. The parser and code generator of the mutator.
- esbuild 0.28.2, MIT license. Build tool only, not in the output.

## Code of the hydra web editor

From https://github.com/hydra-synth/hydra (the editor of hydra.ojack.xyz), commit 680273b, AGPL-3.0:

- js/hydra-editor/Mutator.js (src/views/editor/randomizer/Mutator.js), with changed imports and two fixes
  that the file header lists.
- js/hydra-editor/UndoStack.js (src/views/editor/randomizer/UndoStack.js), unchanged.
- js/hydra-editor/examples.json (src/stores/examples.json), the examples of the "random example" button.

## Algorithms from other projects

- src/audio.c computes the "loudness" feature as Meyda 5.6.3 (MIT) does, so that `a.setScale` and `a.setCutoff`
  keep their meaning. No Meyda code is included.

## Fonts

The code overlay and the 2D canvas use fonts that are installed on the system (default: DejaVu Sans Mono).
No font is included.
