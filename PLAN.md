# Plan

Work for later versions of hydra-native.

## Build without pnpm

Goal: a plain `make` needs no Node.js and no pnpm. Only the tests keep them.

The build gets two things from npm today. CMake can download each of them with a SHA-256 hash, as it does for the
C libraries in vendor/:

- the source of hydra-synth at the commit in package.json (a tarball of GitHub);
- the esbuild binary of the system (the npm tarball of @esbuild/linux-x64, @esbuild/darwin-arm64 or
  @esbuild/win32-x64, in one version).

The bundle then uses `--alias:hydra-synth=vendor/hydra-synth`. The bundle has no other npm package (see
build/hydra-bundle.meta.json).

socket.io, the package of test/signalling_server.cjs, gets its own package.json and lockfile in test/.

Then remove the root package.json, pnpm-lock.yaml and pnpm-workspace.yaml, and the pnpm steps of the macOS job in
.github/workflows/build.yml. Update README.md ("Build"), THIRD_PARTY.md and the license list of tools/package.sh,
which reads the esbuild metafile.

## Before the repository becomes public

The AGPL-3.0 requires the complete source for each person who gets the program. Check these points:

- The release archive contains the source of hydra-native (`source/`), but not the source of the C libraries,
  which CMake downloads. A public repository with the pinned URLs and hashes covers this; else attach a source
  archive with vendor/ to each release.
- licenses/ has no text of the Unicode License v3 (src/unicode_tables.h) and of the MIT license of CodeMirror 5
  (code ported in src/editor.c). THIRD_PARTY.md names both.

## Patch bay on the WebRTC of GStreamer

hydra-native-patch-bay (src/patch_bay_main.c) replaces the Node.js helper. These points are open:

1. A stream with the web editor at https://hydra.ojack.xyz in both directions, by hand (a step for the wizard
   tools/release-tests.sh). This is the step with the most risk: the message formats of simple-peer, and the
   offer of a browser (audio, other payload types). The helper uses only the STUN server of simple-peer; the
   TURN servers that hydra-server sends in `ready` are not used yet.
2. macOS: no system GStreamer. The patch bay there needs GStreamer from Homebrew, or stays out of the app;
   decide after point 1.
