# hydra-native: plan and feasibility

## Question

Is a native Hydra live-coding program feasible on this machine, and is it faster than Hydra in Chromium (Brave)?
The user also wants the features of the official editor (hydra.ojack.xyz): an in-window code editor with the keys of the Pulsar keymap.

## Verdict

At 1920x1080 (the resolution of this laptop), native is not faster than Brave.
In alternating runs the medians were 166 against 192 fps (2609-artrio.js) and 238 against 264 fps (25.11.08-daz-kunsthaus.js).
These differences are smaller than the run-to-run noise (up to 30 %).
At 3840x2160 native is 1.2 to 1.8 times faster (41 against 35 fps, 58 against 32 fps).
Native starts in 0.45 s and uses 115 MB of memory, against about 1 GB summed over the 11 processes of Brave.

A native renderer is feasible: the MVP in this directory runs the repo sketches with the original hydra-synth code.
It is not a reason to leave Chromium for frame rate at 1920x1080 on 60 Hz.
Both variants draw the same shaders on the same Mesa driver, and both reach about 2.5 to 5 times the 60 fps that the display needs.

Update: the user chose an in-window editor with its core in C, with the features of the web editor at hydra.ojack.xyz.
It now exists; see "Parity with hydra.ojack.xyz" for each feature and "Editor, code overlay and keys" for the design.
With the overlay visible, a repo sketch at 1920x1080 renders at 258 fps instead of 271 fps
(0.19 ms more per frame, of which 0.14 ms CPU), measured on a quiet machine.

My recommendation: keep the Chromium variant (hydra/hydra-editor) as the main tool.
Continue hydra-native only if 4K output, a smaller memory footprint or a browser-free stage setup becomes a real need.

## Measurements

Machine: Intel Iris Xe (TigerLake GT2), Mesa, GNOME 50 on Wayland, display 1920x1080 at 60 Hz, scale 1.25.
Brave 1.100 (Chromium 156) uses ANGLE on the Mesa GLES driver; hydra-native uses the Mesa GLES driver directly.
Both run hydra-synth 1.4.0 with `precision mediump` (the hydra default on Linux).
The sketches are the repo files, unchanged. `bench/prelude.js` replaces screen capture, extensions and audio with stubs.
All numbers are frames per second without vsync, so they show the headroom above 60 Hz.

Method: Brave counts `requestAnimationFrame` calls with `--disable-gpu-vsync --disable-frame-rate-limit`,
and Chromium limits the frames in flight, so the count follows the GPU.
hydra-native renders in a hidden window and calls `glFinish` every 30 frames and at the end, so it counts rendered frames.
`bench/compare.sh` alternates the two programs.

Baseline on a quiet machine (Brave alone, first runs):

| Sketch                    | Brave headless 1080p | Brave window 1080p | Brave headless 4K | native window 1920x1015 |
| ------------------------- | -------------------- | ------------------ | ----------------- | ----------------------- |
| 2609-artrio.js            | 216 to 229           | 231                | 32                | 286                     |
| 25.11.08-daz-kunsthaus.js | 299                  | 292                | 107               | 389                     |
| `osc().out()`             | 1202                 |                    |                   |                         |

Alternating runs, while a video player used 4 CPU cores on the same machine (medians of 3 runs at 1080p, 1 run at 4K):

| Sketch                    | Size      | native | Brave | single runs, native / Brave     |
| ------------------------- | --------- | ------ | ----- | ------------------------------- |
| 2609-artrio.js            | 1920x1080 | 166    | 192   | 248 / 193, 166 / 190, 164 / 192 |
| 25.11.08-daz-kunsthaus.js | 1920x1080 | 238    | 264   | 218 / 267, 238 / 264, 240 / 243 |
| 2609-artrio.js            | 3840x2160 | 41     | 35    |                                 |
| 25.11.08-daz-kunsthaus.js | 3840x2160 | 58     | 32    |                                 |

Other numbers:

- Two native runs of daz-kunsthaus, one directly after the other, gave 307 and 174 fps. The machine noise is large.
- JavaScript per frame: QuickJS needs 0.4 to 0.5 ms; V8 in Brave needs less than its timer resolution (0.1 ms).
- Start to first displayed frame: 0.45 s native, including the window and the hydra-synth bundle.
- Memory: native 115 MB RSS. Brave about 1060 MB RSS summed over 11 processes (shared pages count more than once).
- Webcam /dev/video0 through GStreamer: MJPEG 1280x720 at 30 fps.
- I did not measure the latency from a key press or a camera frame to the display.

Conclusion: at 1080p and 60 Hz both variants reach 2.5 to 5 times the needed 60 fps, and the GPU work is the same.
A native renderer only helps at high resolutions, where Chromium's per-frame canvas copy costs GPU bandwidth.

## Where native can win, and where it cannot

Native cannot win in the fragment shaders.
Hydra renders up to four outputs per frame, each one full-screen pass of a generated shader.
At 1920x1080 this cost is the same in both variants, because ANGLE in Brave also passes the GLSL to the Mesa GLES driver.
The JavaScript part of a frame is small: QuickJS (an interpreter) needs about 0.5 ms per frame for the repo sketches.

Native can win in these areas:

- GPU memory bandwidth at high resolutions. Chromium copies the canvas into its compositor every frame.
  At 3840x2160 this copy is a large part of the frame (see the 4K numbers).
- Video input. Brave decodes and copies each video frame through its media pipeline.
  Native can import a PipeWire DMA-BUF or a V4L2 buffer directly as a GL texture (zero copy).
  The MVP does not do this yet: it reads RGBA frames from a pipe (about 3.7 MB per 1280x720 frame).
- Inputs without dialogs. The Mutter ScreenCast API gives monitor, area and window streams without the portal dialog.
  The Chromium variant gets the same through its /capture MJPEG helper, at the cost of a JPEG encode and decode per frame.
- Start time, memory and process count. One process of about 100 MB instead of a browser with many processes.
- Frame pacing and latency. A native window can present with vsync directly; Chromium adds its compositor.
  I did not measure the latency from input to display.

## Architecture

```text
editor --(Unix socket, code)--> hydra-native (C, one process)
                                  SDL2 window, OpenGL ES 3.0 context (Wayland)
                                  QuickJS: build/hydra-bundle.js
                                     hydra-synth 1.4.0, unchanged (npm package)
                                     js/regl-shim.js   regl subset on native GL calls
                                     js/dom-shim.js    window, document, timers, events
                                     js/meyda-shim.js  audio features from src/audio.c
                                     js/sources.js     s0..s3 inputs on src/video.c
                                     js/canvas.js      2D canvas on src/canvas2d.c (plutovg)
                                     js/midi.js        Web MIDI on src/midi.c (ALSA)
                                     js/editor-actions.js  examples and mutator of the web editor
                                  src/video.c   child process -> pipe -> texture upload
                                  src/audio.c   PulseAudio/PipeWire -> FFT -> 24 Bark bands
                                  src/fetch.c   curl for loadScript(url)
                                  src/editor.c, ui.c, overlay.c, keymap.c   the code overlay
```

The design reuses hydra-synth instead of a reimplementation of the API.
esbuild bundles the original `hydra-synth/src/hydra-synth.js` and replaces only three of its dependencies:
`regl` (WebGL wrapper), `raf-loop` (frame loop) and `meyda` (audio features).
Thus the GLSL generator, the transforms, `setFunction`, the array functions (`.fast()`, `.smooth()`), `hush`, `render`,
`setResolution`, the output ping-pong and the audio object `a` are the code of the browser version.
The regl shim is 117 lines.
It compiles shaders when hydra creates a pass, so that a shader error keeps the old pass, as in the browser.

The audio code computes the Meyda "loudness" feature exactly as Meyda 5.6.3 does (512-sample Hann window, 24 Bark bands, sum^0.23).
Thus `a.setScale`, `a.setCutoff` and `a.setBins` keep their meaning.

## Parity with hydra.ojack.xyz

The list comes from the source of the web editor (src/views/editor/editor.js, keymaps.js, src/stores/store.js,
editor-store.js, gallery-store.js, src/views/toolbar.js, Hydra.js, repl-v2.js and randomizer/Mutator.js) and from
the CodeMirror 5 sublime keymap that it loads. Status "done" means that a test or a screenshot checked it.

### Evaluation, overlay and the keys of the web editor

| Feature                          | Web editor                   | hydra-native                       | Status |
| -------------------------------- | ---------------------------- | ---------------------------------- | ------ |
| eval line                        | Ctrl+Enter (line)            | Ctrl/Shift+Enter, also selection   | done   |
| eval block                       | Alt+Enter                    | Alt+Enter                          | done   |
| eval all                         | Ctrl+Shift+Enter, run button | also Ctrl+Alt+Enter, run button    | done   |
| flash of the evaluated code      | 300 ms                       | 300 ms                             | done   |
| error line at the bottom         | yes                          | yes, also shader errors            | done   |
| hide code                        | Ctrl+Shift+H                 | Ctrl+Shift+H                       | done   |
| format code (js-beautify)        | Ctrl+Shift+F                 | Ctrl+Shift+F, Shift+Alt+F (spaces) | done   |
| toggle comment                   | Ctrl+/                       | Ctrl+/ (Ctrl+Shift+7 on ch)        | done   |
| screencap with code              | Ctrl+Shift+S (png and js)    | Ctrl+Shift+S, to ~/Downloads       | done   |
| clear all (hush, speed 1)        | toolbar                      | toolbar, Ctrl+Shift+Delete         | done   |
| random example (examples.json)   | toolbar                      | toolbar, F7                        | done   |
| mutator, undo, change a function | dice, Shift+dice, Meta+dice  | toolbar, F8, Shift+F8, Ctrl+F8     | done   |
| info panel                       | toolbar                      | help of all keys, F1, ? button     | done   |
| line wrapping                    | on                           | on, Alt+Z toggles                  | done   |
| multiple selections              | CodeMirror                   | yes                                | done   |
| code from the URL, save to URL   | Ctrl+Shift+L                 | files on disk instead              | out    |
| share to the gallery             | Ctrl+Shift+G, toolbar        | none, needs the gallery server     | out    |
| extensions panel                 | toolbar                      | none, needs the catalog server     | out    |
| language selection               | info panel                   | English only                       | out    |

### Keys of the CodeMirror 5 sublime keymap (loaded by the web editor)

| Feature                                  | Web editor                  | hydra-native                     | Status  |
| ---------------------------------------- | --------------------------- | -------------------------------- | ------- |
| select next occurrence                   | Ctrl+D                      | Ctrl+D                           | done    |
| select line                              | Ctrl+L                      | Ctrl+L                           | done    |
| delete line                              | Ctrl+Shift+K                | Ctrl+Shift+K                     | done    |
| duplicate line                           | Ctrl+Shift+D                | Ctrl+Shift+D, Shift+Alt+Down     | done    |
| join lines                               | Ctrl+J                      | Ctrl+J                           | done    |
| go to bracket, select between brackets   | Ctrl+M, Ctrl+Shift+M        | Ctrl+M, Ctrl+Shift+M             | done    |
| split selection by line                  | Ctrl+Shift+L                | Ctrl+Shift+L                     | done    |
| add cursor above and below               | Ctrl+Alt+Up/Down            | also Ctrl+Shift+Up/Down (Pulsar) | done    |
| swap line up and down                    | Ctrl+Shift+Up/Down          | Alt+Up/Down (Pulsar)             | done    |
| indent more and less                     | Ctrl+], Ctrl+[              | Ctrl+], Ctrl+[                   | done    |
| transpose characters                     | Ctrl+T                      | Ctrl+T                           | done    |
| sort lines                               | F9                          | F9                               | done    |
| delete word left and right               | Ctrl+Backspace, Ctrl+Delete | Ctrl+Backspace, Ctrl+Delete      | done    |
| move by subword                          | Ctrl+Left/Right             | by word, like Pulsar and VS Code | differs |
| bookmarks, find under, select scope      | Ctrl+F2, Ctrl+F3, ...       | none                             | missing |
| upper and lower case, wrap lines (Alt+Q) | Ctrl+K chords, Alt+Q        | none                             | missing |

### Hydra API and inputs

| Feature                           | Web editor                  | hydra-native                     | Status  |
| --------------------------------- | --------------------------- | -------------------------------- | ------- |
| sources, transforms, render, hush | hydra-synth                 | the same hydra-synth 1.4.0       | done    |
| speed, bpm, update, time, mouse   | hydra-synth                 | the same                         | done    |
| setResolution                     | stretched by CSS            | stretched over the window        | done    |
| top-level await                   | async wrapper               | global async eval                | done    |
| loadScript (paths and http URLs)  | script tag                  | file or curl, global eval        | done    |
| initCam                           | getUserMedia                | GStreamer v4l2src                | done    |
| initScreen                        | portal dialog               | Mutter ScreenCast, no dialog     | done    |
| initVideo, initImage              | video and img elements      | ffmpeg (YouTube needs yt-dlp)    | done    |
| initCanvas and 2D canvas          | DOM canvas                  | plutovg, no gradients            | done    |
| initStream (webRTC)               | patch-bay server            | none                             | out     |
| a (audio FFT, show, setBins, ...) | Meyda on the microphone     | the same feature from PulseAudio | done    |
| Web MIDI input                    | navigator.requestMIDIAccess | ALSA sequencer; no MIDI output   | done    |
| screencap()                       | download                    | PNG and js in ~/Downloads        | done    |
| P5 (p5.js as a source)            | window.P5                   | none                             | missing |
| video recording (vidRecorder)     | MediaRecorder               | none                             | missing |

These parts of the browser behave differently:

- Timers fire at frame boundaries (resolution 16 ms at 60 Hz).
- Keys go to the overlay first, then to the `window` listeners of sketches, as in the browser.

## Video input

The MVP has one generic mechanism: a child process writes raw RGBA frames of a fixed size to a pipe.
A reader thread keeps the newest frame, and the render thread uploads it with `glTexSubImage2D`.
This covers webcams (`gst-launch-1.0 v4l2src`), monitors, areas and windows (`tools/screencast.py`, Mutter ScreenCast),
files and network streams (ffmpeg) and YouTube (`yt-dlp | ffmpeg`, yt-dlp is not installed).

Measured: the webcam /dev/video0 gives MJPEG 1280x720 at 30 fps through this path.
A monitor stream gives no frame until something on that monitor repaints; window and area streams start at once.

Next steps for less CPU and latency:

1. In-process PipeWire with DMA-BUF import through `EGL_EXT_image_dma_buf_import` (zero copy). This needs the PipeWire
   headers: `sudo apt install libpipewire-0.3-dev libspa-0.2-dev`. The runtime library is installed.
2. V4L2 directly: MJPEG decode with libjpeg (installed with headers) or VA-API, YUYV conversion in a shader.
3. ffmpeg with VA-API decode for video files.

## Audio input

`src/audio.c` reads 512-sample mono buffers at 48 kHz with pa_simple from the default source.
`--audio @DEFAULT_MONITOR@` analyzes the system output instead of the microphone.
The browser applies automatic gain control to getUserMedia audio and hydra-native does not, so levels differ.

## Editor, code overlay and keys

The editor is C code in four files:

- `src/editor.c`: the text core. A UTF-8 byte buffer with a line index, a list of selections (multiple cursors),
  and undo by snapshots of the text and of all selections (typing in sequence is one step, 1000 steps).
  Each edit and motion runs once per selection. Line commands, typing rules (closing brackets and quotes,
  indentation), find and replace, block detection, bracket match, words for autocomplete and soft wrap with a
  hanging indent of 2. It has no SDL or GL dependency; `test/editor_test.c` checks it.
- `src/overlay.c`: rendering. stb_truetype packs the glyphs of a monospace font into one atlas texture.
  All rectangles and glyphs of a frame go into one vertex buffer and one draw call, in a vertex array object of its own.
- `src/ui.c`: the overlay state and the commands. Visual row layout, scrolling, mouse (Alt+click adds a cursor,
  Shift+Alt+drag selects a column), clipboard (SDL), prompts for find, replace, go to line and the command palette,
  the autocomplete list, evaluation with flash and error bar, toolbar, help, save, file reload for `--watch`,
  JavaScript syntax colors and bracket pair colors.
- `src/keymap.c`: all keys in one table, with a context for the autocomplete list and the prompts.
  The help (F1) and the command palette list the commands of this table. `test/ui_test.c` sends key events
  through this table, without a window.

Decisions:

- Code from the socket does not enter the editor text. The overlay shows the sketch file; socket errors show in the error bar.
- `--watch` replaces the overlay text only when it has no unsaved edits.
- Only the left Alt key counts as Alt, so AltGr still types characters on the ch and us+intl layouts.
- The formatter is js-beautify with the options of the web editor, but with two spaces instead of tabs.
- Line commands with several cursors work on the lines from the first to the last cursor.
- The editor core is my own code. I compared it with lite-xl and kilo, but no code of them is in it;
  the borrowed parts are listed in THIRD_PARTY.md.

Not done:

- Code folding. It does not combine well with soft wrap and was not cheap.
- tree-sitter for highlighting and indentation. The own highlighter covers JavaScript well enough for sketches.
- Tabs show as one column. The editor inserts two spaces for Tab.
- The preview of an IME composition (SDL_TEXTEDITING). Dead keys and composed characters arrive as text input.
- I did not try typing, dead keys and the mouse by hand: no input could be sent to the desktop without risk to the user's open windows.
  The key table and the commands are checked by `test/ui_test.c`; the rendering by screenshots (`--screenshot`).

Custom keys: hydra-native sends DOM-like `keydown` and `keyup` events to `window` listeners,
so sketches and loaded scripts define keys with the browser API.

## Effort estimate

| Work package                                                     | Effort      | State         |
| ---------------------------------------------------------------- | ----------- | ------------- |
| Baseline measurement in Brave                                    | 0.5 day     | done          |
| MVP: QuickJS, hydra-synth, regl shim, window, socket, watch      | 1 day       | done          |
| MVP: webcam, monitor, area, window, video inputs; audio FFT      | 0.5 day     | done          |
| Pulsar sender package (eval line, block, all)                    | 0.2 day     | done, untried |
| In-window editor in C: overlay, editing, eval keys, files        | 2 days      | done          |
| Web editor parity: toolbar, examples, mutator, format, screencap | 1 day       | done          |
| Multiple cursors, wrap, find and replace, palette, autocomplete  | 2 days      | done          |
| Web MIDI input (ALSA sequencer)                                  | 0.5 day     | done          |
| 2D canvas with plutovg                                           | 0.5 day     | done          |
| p5.js as a source (needs more of the 2D canvas and DOM)          | 3 to 5 days | open          |
| Zero-copy PipeWire and V4L2 input (needs PipeWire headers)       | 3 to 5 days | open          |
| Robustness for stage use (error overlay, crash recovery, tests)  | 3 to 5 days | open          |

## System packages

No new system package is needed. These optional packages enable the next steps:

```sh
sudo apt install libpipewire-0.3-dev libspa-0.2-dev   # zero-copy screen capture
pipx install yt-dlp                                  # YouTube input (no sudo)
```
