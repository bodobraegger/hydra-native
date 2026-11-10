# hydra-native

hydra-native runs Hydra sketches in a native window without a browser.
It runs the unchanged hydra-synth 1.4.0 code in the QuickJS engine.
A small regl replacement draws with OpenGL ES through SDL2.
A code editor in C draws the sketch over the visuals, like the editor of hydra.ojack.xyz.
Video comes from child processes (GStreamer, ffmpeg, the Mutter ScreenCast API) and audio comes from PulseAudio or PipeWire.
See [PLAN.md](PLAN.md) for the measurements, the design and the open work.

## Build

The build needs gcc, make, pnpm, curl and the development files of SDL2, OpenGL ES 2 and libpulse.
These are installed on this machine.
On the first run, `make` downloads QuickJS 2026-06-04 and stb_truetype.h (a pinned commit) into vendor/.

```sh
cd hydra/hydra-native
pnpm install     # hydra-synth 1.4.0 and esbuild
make             # build/hydra-native and build/hydra-bundle.js
make test        # checks of the editing core and of the key table (no window)
make check       # make test, then renders two outputs in a hidden window and reads pixels back
```

`make check` prints `editor_test: ok`, `ui_test: ok` and `{"check":"ok",...}`, and exits with status 0.

## Run

```sh
build/hydra-native sketch.js                    # window, vsync, code overlay, code socket
build/hydra-native --fullscreen setup.js sketch.js
build/hydra-native --watch sketch.js            # load and evaluate sketch.js again when another program saves it
build/hydra-native --audio @DEFAULT_MONITOR@ sketch.js   # FFT of the system output instead of the microphone
build/hydra-native --font /usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf --font-size 24 sketch.js
build/hydra-native --screenshot shot.png sketch.js       # write the window with the code as PNG after 1.5 s and exit
build/hydra-native 'https://hydra.ojack.xyz/?code=...'  # a sketch URL of the web editor
build/hydra-native --lang de                             # the info panel in German
build/hydra-native --gallery-server URL sketch.js        # Ctrl+Shift+G shares with this hydra gallery server
```

The program evaluates the files in the given order, in the global scope, like the browser console.
Without a file, it starts like the web editor: with a random example and the info panel.
The last file opens in the code overlay, and Ctrl+S saves the overlay text to it.
If the file does not exist yet, the overlay starts empty and Ctrl+S creates the file.
With `--watch`, a change on disk replaces the overlay text and evaluates it, but only when the overlay has no unsaved edits.
Otherwise the error bar says that the file changed, and the edits stay.
Run `build/hydra-native --help` for all options.

## Code overlay

The overlay shows the code on a dark, partly transparent background behind each line, with syntax colors
and bracket pair colors. Long lines wrap with a hanging indent of 2 (Alt+Z toggles the wrap).
Evaluated lines flash for 300 ms.
An evaluation error, or a console warning during the evaluation (for example a shader compile error), shows in the bar at the bottom.
The next successful evaluation clears the bar.
Ctrl+Shift+H hides and shows the overlay; the visuals keep running, and the evaluation keys keep working.
The default font is DejaVu Sans Mono at 20 pixels (`--font`, `--font-size`, Ctrl+plus and Ctrl+minus).
The toolbar at the top right has the buttons of the web editor: run, clear, extensions, example, mutate
(Shift: undo, Ctrl: change a function), share (with `--gallery-server`) and info. The info panel has the
languages of the web editor (from hydra-synth/l10n, cached in ~/.cache/hydra-native/l10n).
The extensions panel lists the catalog of hydra-synth/hydra-extensions; "+" adds the load code to the top.
Ctrl+Shift+L copies the hydra.ojack.xyz URL of the code. PLAN.md compares every feature with the web editor.

The editor has multiple cursors, find and replace, go to line, a command palette and autocomplete of hydra
names and of the words of the file. Typing an opening bracket or quote adds the closing one. Find works like
the find bar of a browser: it ignores case and accents, highlights all matches and shows their count.

## Send code from an editor

hydra-native listens on the Unix socket `$XDG_RUNTIME_DIR/hydra-native.sock`.
A client sends JavaScript, closes its write side and gets `ok` or the error with its stack.
Code from the socket does not enter the overlay text, because the overlay shows the sketch file.
Errors of socket code show in the error bar with the prefix `socket:`.

```sh
echo 'osc(20, 0.1, 1.2).kaleid(5).out()' | build/hydra-native --send
socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/hydra-native.sock < block.js
```

From Neovim or Vim, send the visual selection with `:'<,'>w !build/hydra-native --send`.
For Pulsar, `pulsar/hydra-native-send` is a small package with the keys of the user's keymap:
Ctrl+Enter and Shift+Enter evaluate the line or the selection, Alt+Enter the block, Ctrl+Alt+Enter and Ctrl+Shift+Enter the file.
Install it with `pulsar -p link pulsar/hydra-native-send` and disable atom-hydra, because both packages use the same keys.
The package is not tested in Pulsar yet.

## Keys

All keys are in one table in `src/keymap.c`; change a key there and run `make`.
F1 shows every command with its keys, and Ctrl+Shift+P finds a command by name.
The main keys follow the web editor, the user's Pulsar keymap and VS Code.
Keys marked "always" also work while the overlay is hidden.

| Key                                            | Action                                                 |
| ---------------------------------------------- | ------------------------------------------------------ |
| Ctrl+Enter, Shift+Enter                        | evaluate the line or the selection (always)            |
| Alt+Enter                                      | evaluate the block between blank lines (always)        |
| Ctrl+Alt+Enter, Ctrl+Shift+Enter               | evaluate all (always)                                  |
| Ctrl+Shift+H                                   | hide or show the code (always)                         |
| Ctrl+S; Ctrl+Shift+S                           | save; save a screenshot and the code (always)          |
| F1; Ctrl+Shift+P                               | help; command palette (always)                         |
| Ctrl+Shift+L; Ctrl+Shift+G                     | copy the sketch URL; share with the gallery (always)   |
| F7; F8, Shift+F8, Ctrl+F8; Ctrl+Shift+Delete   | example; mutate, undo, change a function; clear all    |
| F11; Ctrl+Q                                    | fullscreen; quit (always)                              |
| Ctrl+Shift+F, Shift+Alt+F                      | format with js-beautify                                |
| Ctrl+/ (Ctrl+Shift+7 on the ch layout)         | toggle comment                                         |
| Alt+Up, Alt+Down; Shift+Alt+Down, Ctrl+Shift+D | move lines; duplicate lines                            |
| Ctrl+L; Ctrl+Shift+K; Ctrl+J                   | select line; delete line; join lines                   |
| Ctrl+D                                         | select the word, then add the next occurrence          |
| Ctrl+Shift+Up/Down, Ctrl+Alt+Up/Down           | add a cursor above or below                            |
| Alt+click; Shift+Alt+drag; Shift+Alt+I         | add a cursor; column selection; a cursor per line      |
| Alt+Left, Alt+Right                            | move by subword (camelCase)                            |
| Ctrl+F2, F2, Shift+F2; Ctrl+F3, Alt+F3         | bookmarks; find or select the word under the cursor    |
| Ctrl+K, then Ctrl+U, L, K, Backspace, D        | the two-key commands of the sublime keymap             |
| Ctrl+F, F3, Shift+F3; Ctrl+H; Ctrl+G           | find; replace (Tab, Enter, Ctrl+Alt+Enter); go to line |
| Ctrl+Space                                     | autocomplete (also while typing a word)                |
| Ctrl+], Ctrl+[; Tab, Shift+Tab                 | indent, outdent                                        |
| Ctrl+M; Ctrl+Shift+M                           | go to the bracket; select inside the brackets          |
| Ctrl+Up, Ctrl+Down                             | nothing                                                |
| Escape                                         | close a popup, keep one cursor, leave fullscreen       |

Sketches add keys with the browser API, for example
`window.addEventListener('keydown', (event) => { if (event.key === '1') speed = 2 })`.
As in the browser, sketches get every key event, also the keys that the overlay uses.

## Inputs

All `init*` calls accept `{ width, height }` for the capture size (default 1280x720) and the regl texture options (`min`, `mag`, `wrap`).

| Call                            | Source                                                        |
| ------------------------------- | ------------------------------------------------------------- |
| `s0.initCam(index)`             | webcam number `index` (V4L2, GStreamer)                       |
| `s0.initScreen()`               | a window or monitor that you pick in the portal dialog        |
| `s0.initMonitor(index)`         | monitor number `index`, no dialog (Mutter ScreenCast)         |
| `s0.initArea(x, y, w, h)`       | an area of the screen, no portal dialog                       |
| `s0.initWindow(delay)`          | the window that has the focus after `delay` seconds           |
| `s0.initVideo(url)`             | a file or stream (ffmpeg, loops); YouTube links need `yt-dlp` |
| `s0.initImage(url)`             | an image (ffmpeg)                                             |
| `s0.initCommand(shell, {w, h})` | any command that writes raw RGBA frames of w x h to stdout    |

`initScreen()` opens the picker of xdg-desktop-portal, as `getDisplayMedia()` does in the browser.
`initMonitor`, `initArea` and `initWindow` use the Mutter ScreenCast API directly and open no dialog.
A monitor stream sends no frame until something on that monitor repaints.
Window and area streams start at once.

`p5 = new P5()` creates a p5.js 1.6.0 sketch, as in the web editor, in 2D mode or with `new P5({ mode: 'WEBGL' })`
in WEBGL mode; `s0.init({ src: p5.canvas })` uses it as a source.
The 2D canvas has gradients, patterns, shadows, filters, image data, images and all blend modes, and it draws
WebGL canvases with `drawImage`. `canvas.getContext('webgl')` gives a WebGL 1 context.
`fetch()` and `XMLHttpRequest` read files and download URLs (GET only), so p5 `loadFont` and `loadJSON` work. `vidRecorder.start()` and `vidRecorder.stop()` record the visuals to a WebM file
(VP9, 25 fps) in ~/Downloads. `navigator.requestMIDIAccess()` gives MIDI input and output through ALSA.

The audio object `a` is the one of hydra-synth (`a.show()`, `a.setBins()`, `a.setSmooth()`, `a.fft`).
Like getUserMedia() of a browser, hydra-native applies noise suppression and automatic gain control to the input
(src/audio_processing.c). These follow the rules of the WebRTC audio processing in Chrome, but they are own code,
so the values can differ a little. Echo cancellation is not necessary, because hydra plays no sound.
Thus `a.setScale()` and `a.setCutoff()` can need other values than in the browser.

## Benchmark

```sh
bench/compare.sh 3 1920 1080     # native and Brave, alternating, same sketches
build/hydra-native --no-vsync --bench 10 bench/prelude.js ../../2609-artrio.js
```

`bench/prelude.js` replaces the screen captures, the extensions and the audio input of the repo sketches with stubs.
