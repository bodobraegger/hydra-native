# hydra-native

hydra-native runs Hydra sketches in a native window without a browser.
It runs the unchanged hydra-synth 1.4.0 code in the QuickJS engine.
A small regl replacement draws with OpenGL ES through SDL3.
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
cd hydra/hydra-native
make             # pnpm install, then build/hydra-native and build/hydra-bundle.js
make test        # unit tests (ctest label "unit"): editing core, key table, audio, MIDI, portal calls
make check       # make test, then the checks in hidden windows (ctest label "check")
```

`make check` exits with status 0 when all tests pass. `ctest --test-dir build -V` shows the output of each test.

## Run

```sh
build/hydra-native sketch.js                    # window, vsync, code overlay, code socket
build/hydra-native --fullscreen setup.js sketch.js
build/hydra-native --watch sketch.js            # load and evaluate sketch.js again when another program saves it
build/hydra-native --audio @DEFAULT_MONITOR@ sketch.js   # FFT of the system output instead of the microphone
build/hydra-native --no-analog-agc sketch.js             # leave the volume of the microphone alone
build/hydra-native --font /usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf --font-size 24 sketch.js
build/hydra-native --screenshot shot.png sketch.js       # write the window with the code as PNG after 1.5 s and exit
build/hydra-native 'https://hydra.ojack.xyz/?code=...'  # a sketch URL of the web editor
build/hydra-native --lang de                             # the info panel in German
build/hydra-native --gallery-server URL sketch.js        # Ctrl+Shift+G shares with this hydra server; pb joins its room
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
An evaluation error, a console warning during the evaluation, or a shader compile error shows in the bar at the bottom.
New shaders compile in the background, and each output shows its old shader until the new one is ready.
Thus an evaluation does not stop the visuals, and a shader compile error comes some frames after the evaluation.
The next successful evaluation clears the bar.
Ctrl+Shift+H hides and shows the overlay; the visuals keep running, and the evaluation keys keep working.
The default font is DejaVu Sans Mono at 20 pixels (`--font`, `--font-size`, Ctrl+plus and Ctrl+minus).
The toolbar at the top right has the buttons of the web editor: run, clear, extensions, example, mutate
(Shift: undo, Ctrl: change a function), share (with `--gallery-server`) and info. The info panel has the
languages of the web editor (from hydra-synth/l10n, cached in ~/.cache/hydra-native/l10n).
The extensions panel lists the catalog of hydra-synth/hydra-extensions; "+" adds the load code to the top.
The window title shows the hydra.ojack.xyz URL of the code after each run of all code, as the address bar of
the web editor does. Ctrl+Shift+L copies that URL, and Ctrl+Shift+O opens the URL in the clipboard.
PLAN.md compares every feature with the web editor.

The "settings" entry of the toolbar (Ctrl+,) chooses the color theme, the autocomplete style and trigger, the
rendering of whitespace, line wrap, the gallery and patch bay server, the font size and a font from the installed
monospace fonts (fc-list). Each choice applies at once and goes into the settings file. The server is "off" (the
default; `pb` is undefined then), hydra.ojack.xyz (https://api.hydrasynth.xyz) or a custom URL; a change starts or
stops the patch bay and the "share" button without a restart. While the patch bay is connected, the panel shows
pb.id and the stream id, which other peers pass to `s0.initStream`; Ctrl+Alt+I copies the stream id.

### Settings file

All settings live in one JSON file: ~/.config/hydra-native/settings.json, or
$XDG_CONFIG_HOME/hydra-native/settings.json. settings.example.json shows every key. hydra-native reads the file at
the start, and command line options override it for one run. The settings menu writes the file atomically (a
temporary file, then rename), and keys that hydra-native does not know stay in it. When the file is no valid
JSON, the defaults apply, the error bar shows the error with its line, and nothing overwrites the file.

| Key                 | Default          | Option               | Meaning                                  |
| ------------------- | ---------------- | -------------------- | ---------------------------------------- |
| font                | DejaVu Sans Mono | --font PATH          | monospace font file                      |
| fontSize            | 20               | --font-size PIXELS   | font size of the overlay                 |
| theme               | "hydra"          | --theme NAME         | built-in or custom theme                 |
| renderWhitespace    | "all"            | settings menu        | none, boundary, selection, trailing, all |
| autocompleteStyle   | "list"           | settings menu        | list or ghost                            |
| autocompleteTrigger | "manual"         | settings menu        | manual (Ctrl+Space) or typing            |
| wordWrap            | true             | settings menu, Alt+Z | soft wrap of long lines                  |
| codeVisible         | true             | --hide-code          | show the code at the start               |
| fullscreen          | false            | --fullscreen         | start in fullscreen                      |
| vsync               | true             | --no-vsync           | wait for the display refresh             |
| watch               | false            | --watch              | reload the sketch on changes             |
| windowWidth         | 1280             | --size WxH           | window width                             |
| windowHeight        | 720              | --size WxH           | window height                            |
| audioSource         | "" (default)     | --audio SOURCE       | audio source for `a`                     |
| analogAgc           | true             | --no-analog-agc      | analog gain control                      |
| language            | "" ($LANG)       | --lang CODE          | language of the info panel               |
| galleryServer       | "" (none)        | --gallery-server URL | gallery and patch bay server             |
| l10nUrl             | "" (l10n)        | --l10n-url URL       | editor.json of a language                |
| socket              | "" (runtime dir) | --socket PATH        | the code socket                          |
| themes              | {}               |                      | custom themes, see below                 |
| keybindings         | {}               |                      | keys of commands, see below              |

"all" is the default of renderWhitespace because Pulsar showed invisible characters (showInvisibles) in the
setup that hydra-native follows. --hidden, --screenshot, --bench and --send act on one run and are no settings.
Options switch a boolean only in one direction; a setting of true for fullscreen, for example, needs the file.

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
it, Escape dismisses it, Alt+] and Alt+[ show the other candidates. Typing an opening bracket or quote adds the closing one. Find works like
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
The main keys follow the web editor, the user's Pulsar keymap and VS Code.
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
| Ctrl+Space; Tab, Escape; Alt+], Alt+[          | autocomplete; accept, dismiss; other ghost candidates  |
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
build/hydra-native-capture (src/capture.h, with the Linux backend src/capture_linux.c on GDBus) does both and
writes the frames, so no Python runs; Python is only for some tests.
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
(VP9, 25 fps) in ~/Downloads. `navigator.requestMIDIAccess()` gives MIDI input and output through ALSA. The
`timeStamp` of a MIDI message is its arrival time in the kernel, in the time of `performance.now()`.

With `--gallery-server URL`, `pb` joins the room 'iclc' of the signalling server at that URL, as the web editor
does with its server, and publishes the output of hydra-native. `pb.list()` shows the peers, and
`s0.initStream(id)` receives a peer, such as a web editor in the same room. node runs the patch bay code of the
web editor in tools/patch-bay.mjs, with the prebuilt WebRTC of @roamhq/wrtc (`pnpm install` downloads it).
test/patch_bay_test.py checks it with two instances and test/signalling_server.cjs, a local copy of the signalling
of hydra-server.

The audio object `a` is the one of hydra-synth (`a.show()`, `a.setBins()`, `a.setSmooth()`, `a.fft`).
Like getUserMedia() in Chrome, hydra-native processes the input with the WebRTC audio processing: a high-pass
filter, echo cancellation, noise suppression (level high) and the adaptive digital gain control of AGC2. It uses
the system library libwebrtc-audio-processing-1.so.3, which PipeWire installs. Without that library,
src/audio_processing.c does a similar noise suppression and gain control.

Also like Chrome on Linux, the analog gain control of that library changes the volume of the capture source in
PulseAudio or PipeWire: up for a quiet voice, down when the signal clips. hydra-native saves the volume when the
audio starts and sets it back when the program ends, also after Ctrl+C (SIGINT) or SIGTERM. A second signal ends
the program at once, without that step. `--no-analog-agc` turns the analog gain control off, and the volume of
the source stays as it is. Monitor sources (`--audio @DEFAULT_MONITOR@`) keep their volume in any case.
Thus `a.setScale()` and `a.setCutoff()` can need other values than in the browser.

## Benchmark

```sh
bench/compare.sh 3 1920 1080     # native and Brave, alternating, same sketches
build/hydra-native --no-vsync --bench 10 bench/prelude.js ../../2609-artrio.js
```

`bench/prelude.js` replaces the screen captures, the extensions and the audio input of the repo sketches with stubs.

`src/programs.c` selects how shaders compile, and the environment variable `HYDRA_SHADER_COMPILE` overrides it:
`thread` (a second GL context on a thread of its own, the default on Mesa), `parallel` (GL_KHR_parallel_shader_compile,
the default with other drivers that have it, for example ANGLE) or `blocking` (the frame waits for the compiler).
