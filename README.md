# hydra-native

hydra-native runs Hydra sketches in a native window without a browser.
It runs the hydra-synth code in the QuickJS engine. A small regl replacement draws with OpenGL ES through SDL3.
A code editor in C draws the sketch over the visuals, like the editor of hydra.ojack.xyz.
Video comes from child processes (GStreamer, ffmpeg, the Mutter ScreenCast API) and audio comes from PulseAudio or PipeWire.
See [PLAN.md](PLAN.md) for the measurements, the design and the open work.

## Build

The build needs gcc, g++, CMake 3.25 or newer, Ninja, make, pnpm and the development files of SDL3, OpenGL ES 2,
libpulse, ALSA and GIO. On Ubuntu 26.04:

```sh
sudo apt install build-essential cmake ninja-build libsdl3-dev libgles-dev libpulse-dev libasound2-dev \
  libgio-2.0-dev libwebrtc-audio-processing-1-3
```

libwebrtc-audio-processing-1-3 (the library, no development files) is optional: without it, the audio input
has no echo and noise filters. At the first configuration, CMake downloads QuickJS, HarfBuzz, SheenBidi, PlutoVG,
stb and the Noto Emoji font into vendor/ and checks the SHA-256 hash of each download. `make` is a thin wrapper
around CMake and Ninja; `BUILD=dir` selects another build directory, which must be a direct child of this folder.

```sh
cd hydra-native
make             # pnpm install, then build/hydra-native and build/hydra-bundle.js
make test        # unit tests (ctest label "unit"): editing core, key table, audio, MIDI, portal calls
make check       # make test, then the checks in hidden windows (ctest label "check")
```

`make check` exits with status 0 when all tests pass. `ctest --test-dir build -V` shows the output of each test.

## Run

```sh
build/hydra-native sketch.js                             # window, vsync, code overlay, code socket
build/hydra-native setup.js sketch.js                    # evaluates setup.js, then shows and evaluates sketch.js
build/hydra-native 'https://hydra.ojack.xyz/?code=...'  # a sketch URL of the web editor
build/hydra-native --fullscreen --audio @DEFAULT_MONITOR@ sketch.js  # settings for this run only
```

The settings menu (Ctrl+,) sets everything that the program can be told: the theme, the font, the window, the
audio input, the language, the server, the reload on file change and more. It saves them in settings.json. Each
of these settings also has a flag that overrides it for one run (see "Settings file"). The flags for scripts and
tests are in "Scripts and tests".

The program evaluates the files in the given order, in the global scope, like the browser console.
Without a file, it starts like the web editor: with a random example and the info panel.
The last file opens in the code overlay, and Ctrl+S saves the overlay text to it.
If the file does not exist yet, the overlay starts empty and Ctrl+S creates the file.
The "file" row of the settings menu shows the open file; a click on it opens a file dialog (on Linux through the
desktop portal) for another .js file, which replaces the code (Ctrl+Z brings the old code back), runs, and
becomes the open file. Without an open file, Ctrl+S asks for a file in a save dialog. The window title shows the
name of the open file and the hydra.ojack.xyz URL of the code.
With the setting "reload on file change" (watch), a change on disk replaces the overlay text and evaluates it,
but only when the overlay has no unsaved edits. Otherwise the error bar says that the file changed, and the edits
stay. Run `build/hydra-native --help` for all flags.

## Scripts and tests

These flags act on one run and are no settings:

- `--send` sends the code on stdin to a running hydra-native through the code socket and prints its answer
  (see "Send code from an editor").
- `--socket PATH` uses another code socket than `$XDG_RUNTIME_DIR/hydra-native.sock` ("socket" in settings.json).
- `--hidden` runs without a visible window, for the checks.
- `--size WxH` sets the window size (default 1280x720), for example for the benchmark.
- `--bench SECONDS` measures the frame rate after a warm-up, prints JSON and exits.
- `--screenshot FILE` writes the window with the code as PNG after 1.5 s and exits.

## Code overlay

The overlay shows the code on a dark, partly transparent background behind each line, with syntax colors
and bracket pair colors. Long lines wrap with a hanging indent of 2 (Alt+Z toggles the wrap).
Evaluated lines flash for 300 ms.
An evaluation error, a console warning during the evaluation, or a shader compile error shows in the bar at the bottom.
New shaders compile in the background, and each output shows its old shader until the new one is ready.
Thus an evaluation does not stop the visuals, and a shader compile error comes some frames after the evaluation.
The next successful evaluation clears the bar.
Ctrl+Shift+H hides and shows the overlay; the visuals keep running, and the evaluation keys keep working.
The default font is DejaVu Sans Mono at 20 pixels (settings menu: font size and font; Ctrl+plus and Ctrl+minus).
The toolbar at the top right has the buttons of the web editor: run, clear, extensions, example, mutate
(Shift: undo, Ctrl: change a function), share (with a server in the settings) and info. The info panel has the
languages of the web editor (from hydra-synth/l10n, cached in ~/.cache/hydra-native/l10n).
The extensions panel lists the catalog of hydra-synth/hydra-extensions; "+" adds the load code to the top.
The window title shows the hydra.ojack.xyz URL of the code after each run of all code, as the address bar of
the web editor does. Ctrl+Shift+L copies that URL, and Ctrl+Shift+O opens the URL in the clipboard.
PLAN.md compares every feature with the web editor.

The "settings" entry of the toolbar (Ctrl+,) chooses the open file and its reload on change, the color theme,
the autocomplete style with its trigger and the signature help, the code display (background opacity and blend,
text blend), the code at start, the rendering of whitespace, line wrap, the server, the window (fullscreen,
vsync), the audio input with its gain, the language, the font size and a font from the installed monospace fonts
(fc-list) or a font file. The rows of a group are indented under it. Each choice applies at once and goes into
the settings file, except "code at start", which applies at the next start. The panel scrolls with the wheel.
"input gain" multiplies the audio input before the analysis of `a` (0 to 400 %, default 100 %); the system
output, which has no automatic gain, often needs more than 100 %. The server is "off" (the
default; `pb` is undefined then), hydra.ojack.xyz (https://api.hydrasynth.xyz) or a custom URL; a change starts or
stops the patch bay and the "share" button without a restart. While the patch bay is connected, the panel shows
the stream id, which other peers pass to `s0.initStream` and which `pb.list()` shows; its "copy" button and
Ctrl+Alt+I copy it.

The numbers of the settings menu (the background opacity, the input gain and the font size) have - and + buttons and can
also be typed: a click on the value turns it into a text field with the value selected. Enter applies the typed
value, also with its unit ("45 %", "20px"), clamped to the valid range; Escape leaves the field without a
change. An entry that is no number shows in the error color and changes nothing. Up and Down, and the mouse
wheel over the value, step it at once, by one or with Shift by ten.

### Settings file

All settings live in one JSON file: ~/.config/hydra-native/settings.json, or
$XDG_CONFIG_HOME/hydra-native/settings.json. settings.example.json shows every key. hydra-native reads the file at
the start, and command line options override it for one run. The settings menu writes the file atomically (a
temporary file, then rename), and keys that hydra-native does not know stay in it. When the file is no valid
JSON, the defaults apply, the error bar shows the error with its line, and nothing overwrites the file.

| Key                   | Default          | Settings menu        | Flag for one run     | Meaning                    |
| --------------------- | ---------------- | -------------------- | -------------------- | -------------------------- |
| font                  | DejaVu Sans Mono | font (list, file...) | --font PATH          | monospace font file        |
| fontSize              | 20               | font size            | --font-size PIXELS   | font size of the overlay   |
| theme                 | "hydra"          | theme                | --theme NAME         | built-in or custom theme   |
| renderWhitespace      | "all"            | whitespace           |                      | none to all                |
| autocompleteStyle     | "list"           | autocomplete         |                      | list or ghost              |
| autocompleteTrigger   | "manual"         | trigger              |                      | manual or typing           |
| signatureHelp         | "manual"         | signatures           |                      | manual or paren            |
| codeBackgroundOpacity | theme alpha      | background opacity   |                      | 0 to 100 percent           |
| codeBackgroundBlend   | "normal"         | background blend     |                      | normal or difference       |
| codeTextBlend         | "normal"         | text blend           |                      | normal or difference       |
| wordWrap              | true             | line wrap, Alt+Z     |                      | soft wrap of long lines    |
| codeVisible           | true             | code at start        | --hide-code          | the code at the start      |
| fullscreen            | false            | window               | --fullscreen         | fullscreen                 |
| vsync                 | true             | window               | --no-vsync           | wait for the refresh       |
| watch                 | false            | reload on change     | --watch              | reload the sketch file     |
| audioSource           | "" (default)     | audio input          | --audio SOURCE       | input of `a`, or "none"    |
| audioGain             | 100              | input gain           |                      | 0 to 400 percent           |
| language              | "" ($LANG)       | language             | --lang CODE          | language of the info panel |
| galleryServer         | "" (none)        | server               | --gallery-server URL | gallery and patch bay      |
| l10nUrl               | "" (l10n)        | translation URL...   | --l10n-url URL       | editor.json of a language  |
| socket                | "" (runtime dir) |                      | --socket PATH        | the code socket (scripts)  |
| themes                | {}               |                      |                      | custom themes, see below   |
| keybindings           | {}               |                      |                      | keys, see below            |

The code overlay can blend with the visuals. "codeBackgroundOpacity" sets the opacity of the box behind each code
line; without it, the theme sets it (72 % in "hydra"). With "codeBackgroundBlend": "difference", the box is white
and inverts the visuals below it, at that opacity. With "codeTextBlend": "difference", the text shows as the
difference of its theme color and the visuals, so it stays readable over bright and over dark visuals. The
selection, the cursor, the flash of an evaluation, the find matches and the bars keep their normal look. Both use
the blend mode difference of the autocomplete list.

"all" is the default of renderWhitespace because Pulsar showed invisible characters (showInvisibles) in the
setup that hydra-native follows. The flags of a boolean switch it only in one direction for the run; the settings
menu sets both.

A custom theme in "themes" has a name, an optional "base" (a built-in theme, "hydra" by default) and colors as
"#rrggbb" or "#rrggbbaa": lineBackground, selection, flash, bracket, cursor, text, keyword, number, string,
comment, function, barBackground, error, info, toolbar, popupBackground, popupSelected, helpBackground,
helpKeys, bookmark, match, matchActive, matchText, ghost, whitespace and "brackets" (a list of three colors).
The built-in themes stay available by name.

"keybindings" maps a command name to a key, such as "Ctrl+Shift+Enter", a two-key command such as
"Ctrl+K Ctrl+U", a list of keys, or "" to unbind it, like keybindings.json of VS Code. The keys replace the
default keys of that command and win over other commands with the same key. F1 shows each command with its
name in brackets and its keys after these changes.

The editor has multiple cursors, find and replace, go to line, a command palette and autocomplete of hydra
names and of the words of the file. Autocomplete opens with Ctrl+Space, or while typing with the trigger "while
typing" of the settings menu. The style "list" shows the candidates below the word, or above its line when there is no
room below, with blend mode difference: a blue box with white text over black, inverted colors over anything
else. The style "ghost
text" shows the rest of the best candidate after the cursor, like the inline suggestions of VS Code: Tab accepts
it, Escape dismisses it, Alt+] and Alt+[ show the other candidates.

The completions follow the type of the place. After a ".", the list has the transforms of a chain (color,
coord, combine and combineCoord functions, and `out`), and no source functions. In a texture argument (`src()`,
the first argument of combine and combineCoord functions), it has o0 to o3, s0 to s3, `src()` and the source
functions. In a number argument, it has the default value, `() => `, `[ ]`, and the helpers of a setup file when
they exist at run time: `cc[]`, `hit()`, `midi.beat`, `midi.bpm`, `time` and `a.fft[]`. A helper goes into an
arrow function, because a plain value would not change. A function from these lists comes with its arguments
and their defaults, for example `modulate(o0, 0.1)`. Tab and Shift+Tab select the next and the previous argument,
the last Tab goes behind the call, and Escape ends the tab stops.

Ctrl+Shift+Space shows the signature help of the hydra function around the cursor, as in VS Code: the call with
its parameters and defaults, the current parameter marked, and its type and default on a second line. It follows
the cursor between the commas, also in nested calls, and closes when the cursor leaves the call or with Escape.
With "signatureHelp": "paren" (settings menu: "on ("), it also opens after typing "(" behind a hydra function. It
has the look of the autocomplete style: a list box with blend mode difference, or ghost text. The definitions
come from the running hydra-synth, so functions of `setFunction` and of extensions appear after their evaluation.
js/api-signatures.js describes the functions outside the glsl list, such as `out`, `render`, `setResolution`,
`hush`, the `init*` sources and `a.setBins`.

Typing an opening bracket or quote adds the closing one. Find works like
the find bar of a browser: it ignores case and accents, highlights all matches and shows their count.

## Send code from an editor

hydra-native listens on the Unix socket `$XDG_RUNTIME_DIR/hydra-native.sock`.
A client sends JavaScript, closes its write side and gets `ok` or the error with its stack.
Code from the socket does not enter the overlay text, because the overlay shows the sketch file.
Errors of socket code show in the error bar with the prefix `socket:`.
The reply comes before the new shaders are compiled, so shader compile errors show only in the error bar and on stderr.

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
Keys marked "always" also work while the overlay is hidden.

| Key                                            | Action                                                 |
| ---------------------------------------------- | ------------------------------------------------------ |
| Ctrl+Enter, Shift+Enter                        | evaluate the line or the selection (always)            |
| Alt+Enter                                      | evaluate the block between blank lines (always)        |
| Ctrl+Alt+Enter, Ctrl+Shift+Enter               | evaluate all (always)                                  |
| Ctrl+Shift+H                                   | hide or show the code (always)                         |
| Ctrl+S; Ctrl+Shift+S                           | save; save a screenshot and the code (always)          |
| F1; Ctrl+Shift+P; Ctrl+,                       | help; command palette; settings (always)               |
| Ctrl+Alt+I                                     | copy the stream id of the patch bay (always)           |
| Ctrl+Shift+L; Ctrl+Shift+O; Ctrl+Shift+G       | copy the URL; open a URL; share with the gallery       |
| F7; F8, Shift+F8, Ctrl+F8; Ctrl+Shift+Delete   | example; mutate, undo, change a function; clear all    |
| F11; Ctrl+Q                                    | fullscreen; quit (always)                              |
| Ctrl+Shift+F, Shift+Alt+F                      | format: indent by brackets and chains, with tabs       |
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
| Ctrl+Space; Tab, Escape; Alt+], Alt+[          | autocomplete; accept, dismiss; other ghost candidates  |
| Ctrl+Shift+Space                               | signature help of the hydra function around the cursor |
| Tab, Shift+Tab; Escape (after a completion)    | next, previous argument of the call; end the tab stops |
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
| `s0.initCam(index)`             | webcam number `index` (GStreamer on Linux, else SDL)          |
| `s0.initScreen()`               | a window or monitor that you pick in the system dialog        |
| `s0.initMonitor(index)`         | monitor number `index`, no dialog                             |
| `s0.initArea(x, y, w, h)`       | an area of the screen, no dialog                              |
| `s0.initWindow(delay)`          | the window that has the focus after `delay` seconds           |
| `s0.initVideo(url)`             | a file or stream (ffmpeg, loops); YouTube links need `yt-dlp` |
| `s0.initImage(url)`             | an image (ffmpeg)                                             |
| `s0.initCommand(shell, {w, h})` | any command that writes raw RGBA frames of w x h to stdout    |

`initScreen()` opens the picker of xdg-desktop-portal, as `getDisplayMedia()` does in the browser.
`initMonitor`, `initArea` and `initWindow` use the Mutter ScreenCast API directly and open no dialog.
build/hydra-native-capture (src/capture.h, with the Linux backend src/capture_linux.c on GDBus) does both and
writes the frames, so no Python runs; Python is only for some tests. On macOS, ScreenCaptureKit and its picker
do the same, and on Windows Windows.Graphics.Capture; PORTING.md describes the permissions and the differences.
initVideo, initImage and vidRecorder need ffmpeg in PATH; the error bar names a missing program.
A monitor stream sends no frame until something on that monitor repaints.
Window and area streams start at once.

`p5 = new P5()` creates a p5.js 1.6.0 sketch, as in the web editor, in 2D mode or with `new P5({ mode: 'WEBGL' })`
in WEBGL mode; `s0.init({ src: p5.canvas })` uses it as a source.
The 2D canvas has gradients, patterns, shadows, filters, image data, images and all blend modes, and it draws
WebGL canvases with `drawImage`. `filter = 'url(#id)'` uses an SVG `<filter>` that a sketch adds to the document
with `innerHTML` or `createElementNS`, with the primitives feGaussianBlur, feColorMatrix, feComponentTransfer,
feOffset, feBlend, feComposite, feFlood, feMerge, feMorphology, feTurbulence and feDisplacementMap. `canvas.getContext('webgl')` gives a WebGL 1 context.
`fetch()` and `XMLHttpRequest` read files and send requests with curl, so p5 `loadFont` and `loadJSON` work.
Requests follow the CORS and mixed-content rules of a page at https://hydra.ojack.xyz, so a server that the web
editor cannot read fails here too. `vidRecorder.start()` and `vidRecorder.stop()` record the visuals to a WebM file
(VP9, 25 fps) in ~/Downloads. `navigator.requestMIDIAccess()` gives MIDI input and output through the ALSA
sequencer (libremidi on macOS and Windows, see PORTING.md). The `timeStamp` of a MIDI message is its arrival time
in the kernel, in the time of `performance.now()`.

With a gallery server (settings menu: server), `pb` joins the room 'iclc' of the signalling server at that URL, as the web editor
does with its server, and publishes the output of hydra-native. `pb.list()` shows the peers, and
`s0.initStream(id)` receives a peer, such as a web editor in the same room. node runs the patch bay code of the
web editor in tools/patch-bay.mjs, with the prebuilt WebRTC of @roamhq/wrtc (`pnpm install` downloads it).
test/patch_bay_test.py checks it with two instances and test/signalling_server.cjs, a local copy of the signalling
of hydra-server.

The audio object `a` is the one of hydra-synth (`a.show()`, `a.setBins()`, `a.setSmooth()`, `a.fft`).
Like getUserMedia() in Chrome, hydra-native processes the input with the WebRTC audio processing: a high-pass
filter, echo cancellation, noise suppression (level high) and the adaptive digital gain control of AGC2. It uses
the system library libwebrtc-audio-processing-1.so.3, which PipeWire installs. Without that library, and on macOS
and Windows, src/audio_processing.c does a similar noise suppression and gain control.

hydra-native does not change the volume of the capture source, which Chrome does on Linux with the analog gain
control of that library. Thus `a.setScale()` and `a.setCutoff()` can need other values than in the browser.

## Benchmark

```sh
bench/compare.sh 3 1920 1080 sketch.js   # native and Brave, alternating, same sketches
build/hydra-native --no-vsync --bench 10 bench/prelude.js sketch.js
```

`bench/prelude.js` replaces the screen captures, the extensions and the audio input of the repo sketches with stubs.

`src/programs.c` selects how shaders compile, and the environment variable `HYDRA_SHADER_COMPILE` overrides it:
`thread` (a second GL context on a thread of its own, the default on Mesa), `parallel` (GL_KHR_parallel_shader_compile,
the default with other drivers that have it, for example ANGLE) or `blocking` (the frame waits for the compiler).

## License

hydra-native is free software under the GNU Affero General Public License 3.0, see [LICENSE](LICENSE).
It contains [hydra-synth](https://github.com/hydra-synth/hydra-synth) and code of the
[hydra web editor](https://github.com/hydra-synth/hydra), both by Olivia Jack and the hydra contributors and
under the same license. [THIRD_PARTY.md](THIRD_PARTY.md) lists all other code and its licenses.
