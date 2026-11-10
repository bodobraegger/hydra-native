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

Update: the user chose an in-window editor with its core in C. It now exists (see "Editor, code overlay and keys").
It has the must-have features: the overlay, editing, the evaluation keys, files and the Pulsar line keys.
It does not have multiple cursors and autocomplete.
With the overlay visible, a repo sketch at 1920x1080 renders at 229 fps instead of 246 fps (0.3 ms more per frame).

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

## Architecture (as built in the MVP)

```text
editor --(Unix socket, code)--> hydra-native (C, one process)
                                  SDL2 window, OpenGL ES 3.0 context (Wayland)
                                  QuickJS: build/hydra-bundle.js
                                     hydra-synth 1.4.0, unchanged (npm package)
                                     js/regl-shim.js   regl subset on native GL calls
                                     js/dom-shim.js    window, document, timers, events
                                     js/meyda-shim.js  audio features from src/audio.c
                                     js/sources.js     s0..s3 inputs on src/video.c
                                  src/video.c   child process -> pipe -> texture upload
                                  src/audio.c   PulseAudio/PipeWire -> FFT -> 24 Bark bands
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

## Compatibility with the Hydra JS API

These parts work as in the browser:

- All sources and transforms of hydra-synth, `out`, `render`, `hush`, `speed`, `bpm`, `update`, `setFunction`.
- `a` with `show`, `hide`, `setBins`, `setSmooth`, `setScale`, `setCutoff`, `fft`, `onBeat`. `a.show()` draws its meter.
- `loadScript(path)` for local files, `setTimeout`, `setInterval`, `requestAnimationFrame`, keyboard and mouse events, `mouse`.
- The extensions hydra-arrays, hydra-blend, hydra-wrap and hydra-tap load and work; `.layer()` of hydra-blend renders.
  hydra-text and hydra-canvas load without errors, but they draw nothing (see below).

These parts do not work:

- 2D canvas drawing (hydra-text, hydra-canvas, `s0.initCanvas()`). There is no 2D canvas implementation.
  Options: a software rasterizer in JS (slow), or a native 2D library (Skia or Cairo, needs `libcairo2-dev`).
- Web MIDI (`navigator.requestMIDIAccess`) in setup-in-console.js. ALSA sequencer input in C is about one day of work.
- `loadScript` of http URLs, `initStream` (peer streams), `screencap` and video recording.
- Top-level `await` in evaluated code.
- Timers fire at frame boundaries (resolution 16 ms at 60 Hz).

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

- `src/editor.c`: the text core. A UTF-8 byte buffer with a line index, one cursor with a selection,
  and undo by snapshots (typing and deleting in sequence merge into one step, 1000 steps).
  Motions, word motion, line operations, comment toggle, format, block detection, bracket match and find.
  It has no SDL or GL dependency; `test/editor_test.c` checks it.
- `src/overlay.c`: rendering. stb_truetype packs the glyphs of a monospace font into one atlas texture.
  All rectangles and glyphs of a frame go into one vertex buffer and one draw call, in a vertex array object of its own.
- `src/ui.c`: the overlay state and the commands. Layout, scrolling, mouse, clipboard (SDL), find,
  evaluation with flash and error bar, save, file reload for `--watch`, JavaScript syntax colors.
- `src/keymap.c`: all keys in one table. `test/ui_test.c` sends key events through this table, without a window.

Decisions:

- Code from the socket does not enter the editor text. The overlay shows the sketch file; socket errors show in the error bar.
- `--watch` replaces the overlay text only when it has no unsaved edits.
- A key event goes to the overlay first and then to the sketch's `window` listeners, as in the browser.
- Only the left Alt key counts as Alt, so AltGr still types characters on the ch and us+intl layouts.
- The format command only changes indentation (bracket depth, one more level for lines that start with ".") and trailing spaces.

Not done:

- Multiple cursors (Shift+Ctrl+Up and Shift+Ctrl+Down). The core has one cursor; Ctrl+D moves the one selection to the next occurrence.
  Multiple cursors need a selection list in every editing operation (estimate: 2 to 3 days).
- Autocomplete of hydra function names (estimate: 1 day; the names come from the transform list of hydra-synth).
- Line wrap. Long lines scroll horizontally.
- Tabs show as one column. The editor inserts two spaces for Tab.
- The preview of an IME composition (SDL_TEXTEDITING). Dead keys and composed characters arrive as text input.
- I did not try typing, dead keys and the mouse by hand: no input could be sent to the desktop without risk to the user's open windows.
  The key table and the commands are checked by `test/ui_test.c`; the rendering by screenshots (`--screenshot`).

Custom keys: hydra-native sends DOM-like `keydown` and `keyup` events to `window` listeners,
so sketches and loaded scripts define keys with the browser API.

## Effort estimate

| Work package                                                    | Effort       | State         |
| --------------------------------------------------------------- | ------------ | ------------- |
| Baseline measurement in Brave                                   | 0.5 day      | done          |
| MVP: QuickJS, hydra-synth, regl shim, window, socket, watch     | 1 day        | done          |
| MVP: webcam, monitor, area, window, video inputs; audio FFT     | 0.5 day      | done          |
| Pulsar sender package (eval line, block, all)                   | 0.2 day      | done, untried |
| In-window editor in C: overlay, editing, eval keys, files       | 2 days       | done          |
| Editor: multiple cursors                                        | 2 to 3 days  | open          |
| Editor: autocomplete of hydra function names                    | 1 day        | open          |
| Zero-copy PipeWire and V4L2 input (needs PipeWire headers)      | 3 to 5 days  | open          |
| Web MIDI replacement (ALSA sequencer)                           | 1 day        | open          |
| 2D canvas for hydra-text and hydra-canvas                       | 1 to 2 weeks | open          |
| Robustness for stage use (error overlay, crash recovery, tests) | 3 to 5 days  | open          |

## System packages

The MVP needs no new system package. These optional packages enable the next steps:

```sh
sudo apt install libpipewire-0.3-dev libspa-0.2-dev   # zero-copy screen capture
sudo apt install libcairo2-dev                       # 2D canvas for hydra-text and hydra-canvas
pipx install yt-dlp                                  # YouTube input (no sudo)
```
