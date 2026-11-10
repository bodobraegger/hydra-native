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
build/hydra-native --screenshot shot.ppm sketch.js       # write the window content after 1.5 s and exit
```

The program evaluates the files in the given order, in the global scope, like the browser console.
The last file opens in the code overlay, and Ctrl+S saves the overlay text to it.
If the file does not exist yet, the overlay starts empty and Ctrl+S creates the file.
With `--watch`, a change on disk replaces the overlay text and evaluates it, but only when the overlay has no unsaved edits.
Otherwise the error bar says that the file changed, and the edits stay.
Run `build/hydra-native --help` for all options.

## Code overlay

The overlay shows the code on a dark, partly transparent background behind each line, with syntax colors.
Evaluated lines flash for 250 ms.
An evaluation error, or a console warning during the evaluation (for example a shader compile error), shows in the bar at the bottom.
The next successful evaluation clears the bar.
Ctrl+Shift+H hides and shows the overlay; the visuals keep running, and the evaluation keys keep working.
The default font is DejaVu Sans Mono at 20 pixels (`--font`, `--font-size`, Ctrl+plus and Ctrl+minus).

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
Keys marked "always" also work while the overlay is hidden.

| Key                                    | Action                                                                    |
| -------------------------------------- | ------------------------------------------------------------------------- |
| Ctrl+Enter, Shift+Enter                | evaluate the line or the selection (always)                               |
| Alt+Enter                              | evaluate the block between blank lines (always)                           |
| Ctrl+Alt+Enter, Ctrl+Shift+Enter       | evaluate all (always)                                                     |
| Ctrl+Shift+H                           | hide or show the code (always)                                            |
| Ctrl+S                                 | save to the sketch file (always)                                          |
| F11                                    | toggle fullscreen (always)                                                |
| Escape                                 | close find, clear the selection, leave fullscreen                         |
| Ctrl+Q                                 | quit (always)                                                             |
| Ctrl+Z; Ctrl+Shift+Z, Ctrl+Y           | undo; redo                                                                |
| Ctrl+C, Ctrl+X, Ctrl+V                 | copy, cut, paste (without a selection: the line)                          |
| Ctrl+A                                 | select all                                                                |
| Ctrl+/ (Ctrl+Shift+7 on the ch layout) | toggle comment                                                            |
| Alt+Up, Alt+Down                       | move the line or the selected lines                                       |
| Shift+Alt+Down                         | duplicate the line or the selected lines                                  |
| Shift+Alt+F                            | format (indentation by brackets and chains)                               |
| Ctrl+D                                 | select the word, then move to the next occurrence                         |
| Ctrl+F                                 | find; Enter finds the next match                                          |
| Ctrl+plus, Ctrl+minus                  | font size                                                                 |
| Ctrl+Up, Ctrl+Down                     | nothing                                                                   |
| Tab, Shift+Tab                         | indent, outdent                                                           |
| Ctrl+Backspace                         | delete the word to the left                                               |
| arrows, Home, End, PageUp, PageDown    | move; with Shift, select                                                  |
| Ctrl+Left, Ctrl+Right, Ctrl+Home/End   | move by word, to the start or end; with Shift, select                     |
| mouse                                  | click, drag, Shift+click, double click (word), triple click (line), wheel |

Sketches add keys with the browser API, for example
`window.addEventListener('keydown', (event) => { if (event.key === '1') speed = 2 })`.
As in the browser, sketches get every key event, also the keys that the overlay uses.

## Inputs

All `init*` calls accept `{ width, height }` for the capture size (default 1280x720) and the regl texture options (`min`, `mag`, `wrap`).

| Call                            | Source                                                        |
| ------------------------------- | ------------------------------------------------------------- |
| `s0.initCam(index)`             | webcam number `index` (V4L2, GStreamer)                       |
| `s0.initScreen(index)`          | monitor number `index`, no portal dialog                      |
| `s0.initArea(x, y, w, h)`       | an area of the screen, no portal dialog                       |
| `s0.initWindow(delay)`          | the window that has the focus after `delay` seconds           |
| `s0.initVideo(url)`             | a file or stream (ffmpeg, loops); YouTube links need `yt-dlp` |
| `s0.initImage(url)`             | an image (ffmpeg)                                             |
| `s0.initCommand(shell, {w, h})` | any command that writes raw RGBA frames of w x h to stdout    |

A monitor stream sends no frame until something on that monitor repaints.
Window and area streams start at once.

The audio object `a` is the one of hydra-synth (`a.show()`, `a.setBins()`, `a.setSmooth()`, `a.fft`).
The browser applies automatic gain control to the microphone, hydra-native does not.
Thus `a.setScale()` and `a.setCutoff()` can need other values than in the browser.

## Benchmark

```sh
bench/compare.sh 3 1920 1080     # native and Brave, alternating, same sketches
build/hydra-native --no-vsync --bench 10 bench/prelude.js ../../2609-artrio.js
```

`bench/prelude.js` replaces the screen captures, the extensions and the audio input of the repo sketches with stubs.
