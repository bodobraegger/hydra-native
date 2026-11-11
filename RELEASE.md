# Release steps

The first version is 0.1.0, with the git tag `v0.1.0`: Linux x86_64, and macOS arm64 as a preview.
A version below 1.0 says that the settings file, the options and the archive layout can still change.

1. Run `make check` on a machine with a GPU. The Linux runner of CI has no GPU for the checks that render in a
   window.
2. Run `tools/release-tests.sh` in a desktop session: it walks through the tests that need a person (the screen
   picker, the file dialogs, the patch bay, the autocomplete box, the macOS app).
3. Tag a commit of main: `git tag -a v0.1.0 -m "hydra-native 0.1.0"`, then `git push origin v0.1.0`.
   The tag must point to a commit on main: the GITHUB_TOKEN cannot create a release for a commit that changes
   .github/workflows relative to main.
4. The workflow `build` makes the Linux archive and the macOS zip and attaches them to a draft release with the
   name of the tag. A failed macOS job does not stop the Linux archive.
5. Copy the approved release notes into the draft, check the attached archives, and publish the draft by hand.

A manual run of the workflow (Actions, build, "Run workflow") makes the same archives as workflow artifacts,
without a release.
`tools/package.sh 0.1.0` makes the archive of this system in dist/, after `make`; its header describes the layout.
`BUILD=dir make` and `HYDRA_NATIVE_BUILD=dir tools/package.sh` use another build directory.

The Linux binary needs glibc 2.43 and SDL3, so it runs on Ubuntu 26.04 and on newer systems.
README.md lists the packages for each feature.

## Release notes (DRAFT, for approval)

> **hydra-native 0.1.0**
>
> hydra-native runs Hydra sketches in a native window on Linux and macOS (preview), without a browser.
> It runs the unchanged hydra-synth code of its main branch (1.4.0 with four fixes), so sketches of
> hydra.ojack.xyz work as they are. A hydra function passed without parentheses, such as
> `osc().modulate(noise)`, shows an error that names it.
> A code editor in the window shows the sketch over the visuals, with the keys, the toolbar and the panels of
> the web editor.
>
> With this release you can:
>
> - write a sketch in the window and evaluate a line, a block or all code, as in the web editor;
>   the old sketch keeps running while the new shaders compile, so an evaluation causes no pause;
> - see the parameters, types and defaults of a hydra function (Ctrl+Shift+Space), get completions that fit the
>   argument (textures, numbers, the transforms of a chain), and insert a call with its defaults as tab stops;
> - open a sketch file or a sketch URL of hydra.ojack.xyz, save to the file, and reload it when another
>   editor saves it;
> - send code from Neovim or another editor through a Unix socket;
> - use a webcam, a video file, an image, the screen, a monitor, an area or a window as a source;
> - use the microphone or the system output for `a`, with noise suppression and automatic gain;
> - use MIDI input and output, with the arrival time of each message, P5 and the 2D canvas;
> - share streams with the web editor and with other hydra-native windows through the patch bay
>   (`pb`, `s0.initStream`);
> - record the visuals as WebM, save screenshots, and share sketches to a hydra gallery server;
> - set the opacity of the code background, and draw the background or the text with the blend mode
>   difference, so the code stays readable over bright and dark visuals;
> - set everything in the settings menu, which saves it in one settings file: the theme, the font (from a list
>   or a font file), the look of the code, fullscreen and vsync, the audio input (a microphone, the system
>   output or none) and its gain, the language, the server, and the reload of the sketch file when another
>   editor saves it. The keys are in the same file.
> - open a sketch file with a file dialog from the settings menu, and save new code to a file with Ctrl+S; the
>   window title shows the name of the file;
> - open sketch URLs, gallery sketches and extensions safely: shared code runs without access to your files
>   and your shell.
>
> Install on Linux: unpack the archive and start `bin/hydra-native`. The folder runs from any place.
> Install on macOS: unzip the archive, move hydra-native.app to Applications, and allow it once in System
> Settings, Privacy and Security ("Open Anyway"), because it has no Apple signature.
> README.md lists the packages for each feature.
>
> Requirements: Ubuntu 26.04 (x86_64) or a newer system with SDL3 and a GPU driver with OpenGL ES 3.0, or
> macOS on Apple Silicon.
>
> Known limits: the macOS app is a preview. There is no Windows build yet. Monitor, area and window capture
> without a dialog work only on GNOME. The patch bay needs Node.js.
>
> License: GNU AGPL 3.0, because hydra-native contains hydra-synth and code of the hydra web editor.
> The archive contains the source of hydra-native and the license texts of the third-party code.
