# hydra-native

hydra-native runs [Hydra](https://hydra.ojack.xyz) sketches in a native window, without a browser.
It has the features of the web editor: the code over the visuals, live evaluation, the gallery, extensions,
cameras, screen capture, audio, MIDI and the patch bay. [docs/manual.md](docs/manual.md) describes all of them.
[What hydra-native leaves out](#what-hydra-native-leaves-out) lists the differences.

## Install

Download the archive for your system from the releases page.

### Linux (x86_64, Ubuntu 26.04 or newer)

```sh
sudo apt install libsdl3-0 libgles2 libpulse0 libasound2t64 libglib2.0-0t64 fontconfig fonts-dejavu-mono curl
tar -xzf hydra-native-*-linux-x86_64.tar.gz
hydra-native-*-linux-x86_64/bin/hydra-native
```

Some inputs need more packages:

- `initCam`: gstreamer1.0-tools and gstreamer1.0-plugins-good.
- `initScreen`, `initMonitor`, `initArea` and `initWindow`: gstreamer1.0-tools, gstreamer1.0-pipewire and
  xdg-desktop-portal. Only `initScreen` works outside of GNOME.
- `initVideo`, `initImage` and `vidRecorder`: ffmpeg. YouTube links also need yt-dlp.
- The patch bay (`pb`, `initStream`): nodejs.
- The links of the info panel: xdg-utils.

The folder runs from any place. The desktop file and the icons are in `assets/`.

### macOS (Apple Silicon, preview)

Unzip `hydra-native-*-macos-arm64.zip` and move `hydra-native.app` to Applications.
The app has no Apple signature. At the first start, macOS blocks it: open System Settings, Privacy and
Security, and click "Open Anyway". For `initVideo` and `initImage`, install ffmpeg (`brew install ffmpeg`).
For the patch bay, install Node.js.

## Run

```sh
hydra-native                                   # a random example, like the web editor
hydra-native sketch.js                         # edit and run sketch.js; Ctrl+S saves it
hydra-native 'https://hydra.ojack.xyz/?code=...'  # a sketch URL of the web editor
```

Ctrl+Enter evaluates the line, Ctrl+Shift+Enter evaluates all, and Ctrl+Shift+H hides the code.
F1 shows all keys. Ctrl+, opens the settings, which go into settings.json.
Sketch URLs, the gallery and extensions run without access to your files and shell (see
[Shared sketches](docs/manual.md#shared-sketches)).

## Build

### Linux

The build needs gcc, CMake 3.25 or newer, Ninja, make, pnpm and these development files (Ubuntu 26.04):

```sh
sudo apt install build-essential cmake ninja-build libsdl3-dev libgles-dev libpulse-dev libasound2-dev \
  libgio-2.0-dev
make            # build/hydra-native
build/hydra-native sketch.js
```

### macOS

```sh
brew install cmake ninja pnpm sdl3
make            # build/hydra-native, with ANGLE for OpenGL ES on Metal
build/hydra-native sketch.js
```

At the first build, CMake downloads the C libraries of [THIRD_PARTY.md](THIRD_PARTY.md) into vendor/ and checks
their SHA-256 hashes.

`make test` runs the unit tests. `make check` also runs the checks in hidden windows, which need a GPU.
`tools/package.sh VERSION` packs the release archive of this system into dist/.

## What hydra-native leaves out

hydra-native does not emulate P5 and the canvas APIs of the browser: the 2D canvas, WebGL contexts for sketches
and SVG filters. An earlier version did. That emulation was about 3,800 lines of code and two dependencies
(the vector graphics library plutovg and p5.js), for a feature that few sketches use. Each of these lines could
differ from the browser in a small way, and all of them needed maintenance on three systems.

The main use of the canvas in hydra sketches is text. Thus `text()`, `strokeText()`, `fillStrokeText()` and
`strokeFillText()` are built in, with the interface of the hydra-text extension (see
[Text](docs/manual.md#text)). They need about 400 lines and no new dependency, because the code overlay
already draws text.

As a result, these sketches do not run:

- sketches that use `P5`;
- sketches that draw on a canvas of their own (`getContext('2d')`, `getContext('webgl')`, `s0.initCanvas()`,
  `s0.init({ src: canvas })`).

In such a sketch, `getContext()` gives `null`, and the error bar shows the error of the first drawing call.

hydra-native also does not apply the CORS rules of a web page to `fetch()`, and settings have no command line
flags: the settings menu and settings.json are the one place for them.

## License

hydra-native is free software under the GNU Affero General Public License 3.0, see [LICENSE](LICENSE).
It contains [hydra-synth](https://github.com/hydra-synth/hydra-synth) and code of the
[hydra web editor](https://github.com/hydra-synth/hydra), both by Olivia Jack and the hydra contributors and
under the same license. [THIRD_PARTY.md](THIRD_PARTY.md) lists all other code and its licenses.
