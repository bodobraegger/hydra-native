# hydra-native

hydra-native runs [Hydra](https://hydra.ojack.xyz) sketches in a native window, without a browser.
It has the features of the web editor: the code over the visuals, live evaluation, the gallery, extensions,
cameras, screen capture, audio, MIDI and the patch bay. [docs/manual.md](docs/manual.md) describes all of them.
It leaves out some browser features, see [below](#what-hydra-native-leaves-out).

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
- `initVideo`, `initImage`, `vidRecorder`, "render to file" and the patch bay (`pb`, `initStream`): ffmpeg.
  YouTube links also need yt-dlp.
- The links of the info panel: xdg-utils.

The folder runs from any place. The desktop file and the icons are in `assets/`.

### macOS (Apple Silicon, preview)

Unzip `hydra-native-*-macos-arm64.zip` and move `hydra-native.app` to Applications.
The app has no Apple signature. At the first start, macOS blocks it: open System Settings, Privacy and
Security, and click "Open Anyway". For `initVideo`, `initImage` and the patch bay, install ffmpeg
(`brew install ffmpeg`).

### Windows (x64, Windows 10 1903 or newer, preview)

Unzip `hydra-native-*-windows-x64.zip` and run `hydra-native.exe` in its `bin` folder.
The program has no signature, so SmartScreen can ask before the first start: click "More info" and "Run anyway".
For `initVideo`, `initImage` and the patch bay, install ffmpeg (`winget install ffmpeg`).

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

The build needs gcc and g++, CMake 3.25 or newer, Ninja, make, pnpm and these development files (Ubuntu 26.04):

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

### Windows

The build needs [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (the UCRT release for x86_64), CMake 3.25
or newer, Ninja, Node.js and pnpm. QuickJS does not build with Visual C++.
Run these commands in Git Bash, with the `bin` folder of llvm-mingw in PATH:

```sh
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build   # build/hydra-native.exe, with ANGLE for OpenGL ES on Direct3D 11
build/hydra-native.exe sketch.js
```

At the first build, CMake downloads the C libraries of [THIRD_PARTY.md](THIRD_PARTY.md) into vendor/ and checks
their SHA-256 hashes.

`make test` runs the unit tests. `make check` also runs the checks in hidden windows, which need a GPU. With
coturn installed (`turnserver` in PATH), the patch bay check also tests a TURN relay.

## What hydra-native leaves out

hydra-native does not emulate `P5` and the canvas APIs of the browser (the 2D canvas, WebGL contexts for sketches,
SVG filters). That emulation was about 3,800 lines of code and two dependencies (plutovg and p5.js) for a feature
that few sketches use, and each line could differ from the browser. The main use of the canvas in sketches is
text, so `text()`, `strokeText()`, `fillStrokeText()` and `strokeFillText()` are built in with the interface of
the hydra-text extension (see [Text](docs/manual.md#text)). They need about 400 lines and no dependency.

Thus sketches that use `P5` or draw on a canvas of their own do not run: `getContext()` gives `null`.
`fetch()` does not apply the CORS rules of a web page.

The editor draws one glyph for each code point, from left to right. It has no right-to-left text (Arabic,
Hebrew), and it does not join the parts of an emoji sequence or place combining marks, because that needed two
dependencies (HarfBuzz and SheenBidi) and the only C++ of the Linux build. An emoji shows in one color, and only
when an installed font has its outline; a color emoji font has pictures only.

## Release

1. Run `make check` on a machine with a GPU, and `tools/release-tests.sh` in a desktop session.
2. Push a tag such as `v0.1.0` on a commit of main. The workflow `build` attaches the Linux archive, the
   macOS app and the Windows zip to a draft release.
3. Write the notes into the draft and publish it.

## License

hydra-native is free software under the GNU Affero General Public License 3.0, see [LICENSE](LICENSE).
It contains [hydra-synth](https://github.com/hydra-synth/hydra-synth) and code of the
[hydra web editor](https://github.com/hydra-synth/hydra), both by Olivia Jack and the hydra contributors and
under the same license. [THIRD_PARTY.md](THIRD_PARTY.md) lists all other code and its licenses.
