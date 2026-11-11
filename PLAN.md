# Plan

Work for later versions of hydra-native.

## Build without pnpm

Goal: a plain `make` needs no Node.js and no pnpm. Only the patch bay keeps them.

The build gets three things from npm today. CMake can download each of them with a SHA-256 hash, as it does for the
C libraries in vendor/:

- the source of hydra-synth at the commit in package.json (a tarball of GitHub);
- the esbuild binary of the system (the npm tarball of @esbuild/linux-x64, @esbuild/darwin-arm64 or
  @esbuild/win32-x64, in one version);
- p5.min.js 1.6.0 (the npm tarball of p5).

The bundle then uses `--alias:hydra-synth=vendor/hydra-synth`. The bundle has no other npm package (see
build/hydra-bundle.meta.json).

The patch bay gets its own package.json and lockfile in tools/, with @roamhq/wrtc, simple-peer,
socket.io-client, shortid and inherits, and socket.io for test/signalling_server.cjs. Only the people who use the
patch bay and the release CI run `pnpm install` there. These files move with it:

- js/hydra-editor/rtc-patch-bay.js and js/hydra-editor/pb-live.js, which only Node.js runs (tools/patch-bay.mjs),
  so that their imports resolve from tools/node_modules;
- the esbuild call and the license list of the patch bay in tools/package.sh.

Then remove the root package.json, pnpm-lock.yaml and pnpm-workspace.yaml, and the pnpm steps of the macOS job in
.github/workflows/build.yml. Update README.md ("Build"), THIRD_PARTY.md and the license list of tools/package.sh,
which reads the esbuild metafiles.
