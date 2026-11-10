# Release plan

This file is the plan for the first release of hydra-native: Linux x86_64 only.
The release notes at the end are a draft. The user approves them before a release is published.

## Version

The first version is 0.1.0, with the git tag `v0.1.0`.
The CMake build of the main branch uses the same project version.
A version below 1.0 says that the settings file, the options and the archive layout can still change.

## Steps to make the release

1. Run `make check` on a machine with a GPU. CI cannot run the checks that render in a window (see "CI review").
2. Tag a commit of main: `git tag -a v0.1.0 -m "hydra-native 0.1.0"`, then `git push origin v0.1.0`.
   The tag must point to a commit on main (see the GITHUB_TOKEN rule in "CI review").
3. The workflow `build` makes the archive and attaches it to a draft release with the name of the tag.
4. Copy the approved release notes into the draft, check the attached archive, and publish the draft by hand.

A manual run of the workflow (Actions, build, "Run workflow") makes the same archive as a workflow artifact,
without a release.
`tools/package.sh 0.1.0` makes the archive locally in dist/, after `make`.
`BUILD=dir make` and `HYDRA_NATIVE_BUILD=dir tools/package.sh` use another build directory.

## Contents of the archive

`hydra-native-0.1.0-linux-x86_64.tar.gz` (about 14 MB) unpacks into one folder:

```text
hydra-native-0.1.0-linux-x86_64/
  bin/hydra-native                   the program
  bin/hydra-native-capture           the screen capture (initScreen, initMonitor, initArea, initWindow)
  bin/libhydra-webrtc-apm.so         the link to the WebRTC audio processing of the system
  bin/hydra-bundle.js, bin/p5.min.js hydra-synth with the shims, and p5.js
  tools/patch-bay.mjs                the webRTC patch bay (pb, initStream), one file for Node.js
  node_modules/@roamhq/wrtc-linux-x64/wrtc.node   the WebRTC module of the patch bay
  vendor/fonts/NotoEmoji.ttf         the emoji font of the overlay
  assets/icon/                       favicon.ico and the hicolor icons (16, 32 and 48 px)
  assets/hydra-native.desktop        the desktop file
  LICENSE                            the GNU AGPL 3.0
  THIRD_PARTY.md, licenses/          the third-party code and its license texts
  source/                            the source of this build and a note where the rest of the source is
```

The program finds its files relative to its own path, also through a symbolic link.
Thus the folder runs from any place.

## Install and run

```sh
mkdir -p ~/.local/opt ~/.local/bin ~/.local/share/applications ~/.local/share/icons
tar -xzf hydra-native-0.1.0-linux-x86_64.tar.gz -C ~/.local/opt
ln -s ~/.local/opt/hydra-native-0.1.0-linux-x86_64/bin/hydra-native ~/.local/bin/hydra-native
cp -r ~/.local/opt/hydra-native-0.1.0-linux-x86_64/assets/icon/hicolor ~/.local/share/icons/
cp ~/.local/opt/hydra-native-0.1.0-linux-x86_64/assets/hydra-native.desktop ~/.local/share/applications/
hydra-native sketch.js
```

The desktop file starts `hydra-native` from the PATH. Ubuntu adds ~/.local/bin to the PATH at the next login.
To remove hydra-native, delete the folder, the link, the desktop file and the three icons.
The settings stay in ~/.config/hydra-native.

## Runtime dependencies on Ubuntu 26.04

Required:

```sh
sudo apt install libsdl3-0 libgles2 libpulse0 libasound2t64 libglib2.0-0t64 fontconfig fonts-dejavu-mono curl
```

The GPU driver must give OpenGL ES 3.0 (Mesa does).

Optional, for each feature:

| Package                                       | Feature                                        |
| --------------------------------------------- | ---------------------------------------------- |
| libwebrtc-audio-processing-1-3                | audio processing as in Chrome (with PipeWire)  |
| gstreamer1.0-tools, gstreamer1.0-plugins-good | initCam                                        |
| gstreamer1.0-tools, gstreamer1.0-pipewire     | initScreen, initMonitor, initArea, initWindow  |
| ffmpeg                                        | initVideo, initImage, vidRecorder              |
| nodejs                                        | the patch bay (pb, initStream); tested with 22 |
| xdg-utils                                     | links of the info panel                        |
| xdg-desktop-portal                            | the picker of initScreen                       |
| yt-dlp (pipx)                                 | YouTube links in initVideo                     |

## Known limits

- Only Linux x86_64. The binary needs glibc 2.43 and SDL3, so it runs on Ubuntu 26.04 and on newer systems.
  Other distributions need a build from the source.
- initMonitor, initArea and initWindow use the ScreenCast API of Mutter, so they work only on GNOME.
- The patch bay needs `node` on the PATH.
- The default font is DejaVu Sans Mono of the system. Without fonts-dejavu-mono, choose a font file in the settings menu.
- The code socket is a Unix socket.
- CI builds the archive and runs the checks without a window. The checks that render run only on a machine with a
  GPU (step 1 above).

## License and source

hydra-native contains hydra-synth and code of the hydra web editor, both under the AGPL-3.0.
Thus the archive is under the AGPL-3.0, and LICENSE in the archive is its text.
The AGPL-3.0 requires that each person who gets the program can also get its complete source.
The archive contains the source of the build commit (`source/`), and source/README.txt says where the
source of hydra-synth and of the web editor is.

Open points for the user:

- The repository is private, so the source in the archive is the only source that a user gets.
  The C libraries (QuickJS, HarfBuzz, plutovg, SheenBidi, stb) are compiled into the binary, but their source
  is not in the archive: CMakeLists.txt downloads them from their upstream URLs. If those URLs can go away, add
  vendor/ to source/ (155 MB uncompressed, 110 MB of it HarfBuzz) or make the repository public.
- The repository has no LICENSE file of its own. THIRD_PARTY.md says that the distributed program is AGPL-3.0.
  A LICENSE file in the repository would make that clear for the source too.
- licenses/ has no text of the Unicode License v3 (src/unicode_tables.h) and of the MIT license of
  CodeMirror 5 (code ported in src/bidi.c and src/editor.c). THIRD_PARTY.md names both.
- wrtc.node contains the WebRTC library of Google, under BSD-3-Clause, with its own third-party code.
  licenses/node/@roamhq+wrtc@0.8.0 has only the license of the Node.js module.

## Release notes (DRAFT, for approval)

> **hydra-native 0.1.0**
>
> hydra-native runs Hydra sketches in a native window on Linux, without a browser.
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
> - use the microphone or the system output for `a`, with the audio processing of Chrome;
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
>   window title shows the name of the file.
>
> Install: unpack the archive and start `bin/hydra-native`. The folder runs from any place.
> RELEASE.md in the repository lists the install steps and the packages for each feature.
>
> Requirements: Ubuntu 26.04 (x86_64) or a newer system with SDL3, and a GPU driver with OpenGL ES 3.0.
>
> Known limits: only Linux x86_64. Monitor, area and window capture without a dialog work only on GNOME.
> The patch bay needs Node.js.
>
> License: GNU AGPL 3.0, because hydra-native contains hydra-synth and code of the hydra web editor.
> The archive contains the source of hydra-native and the license texts of the third-party code.

## CI review

The user decided that the workflow must not run before the review. No run, no push and no local runner
(act, Docker) tested it. This section checks each part of .github/workflows/build.yml against the
documentation at the pinned versions. "Tested locally" means a manual run of the same commands on this
Ubuntu 26.04 machine, outside of CI.

### Triggers

- `on.workflow_dispatch` and `on.push.tags: ['v*']`, without `branches`. When a push filter defines only
  tags, the workflow does not run for branch pushes: "the workflow won't run for events affecting the
  undefined Git ref".
  https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#onpushbranchestagsbranches-ignoretags-ignore
- workflow_dispatch starts a workflow "only if the workflow file exists on the default branch".
  Thus the first manual run is possible only after the workflow is on main.
  https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#workflow_dispatch
- A push of more than three tags at once creates no event, so push the release tag alone.
  Same page, section "push".

### Permissions

- The top level sets `contents: read`, which actions/checkout v7.0.1 recommends ("Recommended permissions"
  in its README). https://github.com/actions/checkout/tree/v7.0.1#recommended-permissions
- The job `release` sets `contents: write` for itself. A job-level `permissions` key configures "a different set
  of permissions for the GITHUB_TOKEN for each job", and all permissions that are not specified become `none`.
  https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#jobsjob_idpermissions
- `POST /repos/{owner}/{repo}/releases` needs Contents write.
  https://docs.github.com/en/rest/authentication/permissions-required-for-github-apps
- The GITHUB_TOKEN cannot create a release whose target commit adds or changes files in .github/workflows
  relative to the default branch. A tag on a commit of main has no such change.
  https://docs.github.com/en/rest/releases/releases#create-a-release
- actions/download-artifact downloads artifacts of the same run without extra permissions; a token is
  necessary only for other runs.
  https://github.com/actions/download-artifact/tree/v8.0.2#download-artifacts-from-other-workflow-runs-or-repositories

### Runner instead of a container

- GitHub has a hosted runner image `ubuntu-26.04` (x64), so the job runs directly on it, with no container.
  https://github.com/actions/runner-images#available-images
- Thus the container points do not apply: the image has git (2.56), sudo, Node.js 24, Python 3.14 and the
  GitHub CLI 2.103, and actions/checkout makes a real git checkout (`git archive` in tools/package.sh needs it).
  https://github.com/actions/runner-images/blob/main/images/ubuntu/Ubuntu2604-Readme.md
- The image ubuntu:26.04 exists on Docker Hub (last update 2026-10-29), if a container becomes necessary.
- The apt step runs `apt-get update` before `apt-get install -y`, with DEBIAN_FRONTEND=noninteractive.
  `sudo env` passes it, because sudo resets the environment.
  The list is the one of README.md ("Build"), plus python3 and alsa-utils for the unit tests.
  All package names exist in Ubuntu 26.04 (checked with `apt-cache policy` on this machine).
  libgio-2.0-dev gives gio-unix-2.0.pc (libglib2.0-dev depends on it).

### Node.js and pnpm

- CMake runs `pnpm install --frozen-lockfile` itself (CMakeLists.txt); the step in the workflow only
  shows its output on its own.
- pnpm/action-setup v6.1.1 with `version: 11` and `cache: true`. The action does not install Node.js
  ("This action does not set up Node.js"), so pnpm runs on the Node.js 24 of the runner image.
  https://github.com/pnpm/action-setup/tree/v6.1.1#readme
- actions/setup-node is not used. The local build uses Node.js 22, CI uses Node.js 24.
- pnpm 11 stops the install when a dependency has build scripts that nobody reviewed (`strictDepBuilds`, default
  true). esbuild is the only dependency with an install script, and pnpm-workspace.yaml lists it with
  `allowBuilds: esbuild: false`, so it counts as reviewed. https://pnpm.io/settings/build
  Tested locally: `pnpm install --frozen-lockfile` with pnpm 11.28.11 exits with 0.

### Artifact

- actions/upload-artifact v7.0.2 with `archive: false` uploads the tar.gz as it is. Then "the name of the file
  will be used as the artifact name", so each matrix job gets its own name, as the README requires
  ("Artifact names must be unique"). The tar.gz keeps the file permissions, which a zip would lose.
  https://github.com/actions/upload-artifact/tree/v7.0.2#readme
- The retention is the default of 90 days.
- actions/download-artifact v8.0.2 saves a file that is not a zip as it is (toolkit:
  packages/artifact/src/internal/download/download-artifact.ts, "Downloading raw file (non-zip)").
  `merge-multiple: true` puts the files of all artifacts into dist/.
  https://github.com/actions/download-artifact/tree/v8.0.2#readme

### Draft release

- `gh release create "$GITHUB_REF_NAME" --draft --verify-tag --title ... --notes ... dist/*.tar.gz`
  with gh 2.103 of the runner image. With `--draft`, "the command leaves the release unpublished"
  (gh v2.103.3, pkg/cmd/release/create/create.go). `--verify-tag` stops when the tag does not exist on GitHub.
  https://cli.github.com/manual/gh_release_create
- gh reads the token from GH_TOKEN and the repository from GH_REPO (gh help environment).
- The tag name comes from GITHUB_REF_NAME, "the short ref name of the branch or tag".
  https://docs.github.com/en/actions/reference/workflows-and-actions/variables
- A second run for the same tag makes a second draft. Delete the extra draft by hand.

### Expressions, conditions and paths

- `${{ matrix.os }}`, `${{ steps.package.outputs.archive }}`, `${{ github.token }}` and
  `${{ github.repository }}` are the only expressions. The versions come from GITHUB_REF_TYPE and
  GITHUB_REF_NAME in the shell, so no `&&`/`||` expression is necessary.
- `if: runner.os == 'Linux'` and `if: github.ref_type == 'tag'`: GitHub evaluates `if` as an expression
  without `${{ }}`. github.ref_type is `branch` or `tag`.
  https://docs.github.com/en/actions/reference/workflows-and-actions/contexts#github-context
- `matrix.include` alone: "If you don't specify any matrix variables, all configurations under include will
  run." A macOS or Windows job is one more entry in `include`.
  https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#jobsjob_idstrategymatrixinclude
- The default shell is `bash -e {0}`, without pipefail. Thus the step "Package" assigns
  `archive=$(tools/package.sh ...)` on its own line, so a failure of the script stops the step.
  Tested locally: with a missing build, the step stops with status 1.
- Paths: all steps run in the checkout. tools/package.sh writes dist/hydra-native-VERSION-linux-x86_64.tar.gz
  and prints its absolute path, which goes to `steps.package.outputs.archive` through GITHUB_OUTPUT.
  Tested locally for a tag run (0.1.0) and for a manual run (the short commit hash).

### Checks on CI

- Run: `make` and `make test`, the ctest label "unit" (test/editor_test.c, ui_test.c, audio_test.c,
  midi_test.c, test/portal_test.py). Tested locally without a display, without PulseAudio and without D-Bus: all pass.
  portal_test.py uses the dry run of hydra-native-capture, which opens no D-Bus connection.
  midi_test.c skips itself when the ALSA sequencer or the Midi Through port is not there.
- Skipped, with a message in the log: the ctest label "check" of `make check` (test/check.js,
  fetch_test.py, gallery_test.py and patch_bay_test.py).
  They start `hydra-native --hidden`, which needs an OpenGL ES context. The runner has no GPU.
  A software renderer is not a safe replacement: locally, SDL_VIDEO_DRIVER=offscreen with the GPU passes
  test/check.js, but with LIBGL_ALWAYS_SOFTWARE=1 the output o0 stays black and the check fails.
- Not run on CI: the test of the archive from another folder (see below).

### Actions, pinned to a commit

- actions/checkout v7.0.1: 3d3c42e5aac5ba805825da76410c181273ba90b1
- pnpm/action-setup v6.1.1: 73e46992e05b19abd31f2f3c20e0e7370d762115
- actions/upload-artifact v7.0.2: cf430e030ddbb5b0abf93d22962f4752f3646cd9
- actions/download-artifact v8.0.2: 9000827ccba6bdab643e8b6fd33ac0654aef8333

### Local test of the archive

The archive was unpacked into a temporary folder, and a symbolic link to bin/hydra-native was started from
another folder. test/check.js passed. strace showed that the program opened the bundle, p5.min.js, the
WebRTC audio library, the emoji font and the icon from the unpacked folder.
test/patch_bay_test.py, changed to start the unpacked program, passed: the patch bay ran with the bundled
tools/patch-bay.mjs and wrtc.node of the archive.
