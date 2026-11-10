#!/bin/sh
# Packs the Linux x86_64 release archive dist/hydra-native-VERSION-linux-x86_64.tar.gz from a finished build.
# usage: tools/package.sh [VERSION]   (default: git describe; run `make` first)
# HYDRA_NATIVE_BUILD selects a build directory other than build/.
#
# The program finds its files relative to its own path: the bundle next to it, and tools/, vendor/fonts/ and
# assets/ one directory up (src/main.c). bin/ keeps that layout, so the unpacked folder runs from any place.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
build=${HYDRA_NATIVE_BUILD:-$root/build}
version=${1:-$(git -C "$root" describe --tags --always --dirty)}
version=${version#v}
name=hydra-native-$version-linux-x86_64
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
target=$stage/$name

mkdir -p "$target/bin" "$target/tools" "$target/vendor/fonts" "$target/assets" "$target/node_modules/@roamhq" \
	"$target/licenses/node" "$target/source"

cp "$build/hydra-native" "$build/hydra-native-capture" "$build/libhydra-webrtc-apm.so" \
	"$build/hydra-bundle.js" "$build/p5.min.js" "$target/bin/"
strip --strip-debug "$target/bin/hydra-native" "$target/bin/hydra-native-capture" "$target/bin/libhydra-webrtc-apm.so"
cp "$root/vendor/fonts/NotoEmoji.ttf" "$target/vendor/fonts/"
cp -r "$root/assets/icon" "$root/assets/hydra-native.desktop" "$target/assets/"

# The patch bay helper as one file, so that only the native WebRTC module comes from node_modules.
# Its require() (for wrtc.node) resolves from tools/, like the require of Node.js in the repo.
"$root/node_modules/.bin/esbuild" "$root/tools/patch-bay.mjs" --bundle --platform=node --format=esm \
	--log-level=warning --banner:js="import { createRequire } from 'node:module'; const require = createRequire(import.meta.url);" \
	--outfile="$target/tools/patch-bay.mjs" --metafile="$stage/patch-bay.meta.json"
cp -rL "$(realpath "$root/node_modules/@roamhq/wrtc")/../wrtc-linux-x64" "$target/node_modules/@roamhq/"

# Licenses: the AGPL-3.0 of the program, the C libraries in the binary, the font, and the npm packages in the archive.
cp "$root/node_modules/hydra-synth/LICENSE" "$target/LICENSE"
cp "$root/THIRD_PARTY.md" "$target/"
licenses=$target/licenses
cp "$root/vendor/quickjs/LICENSE" "$licenses/quickjs.txt"
cp "$root/vendor/plutovg/LICENSE" "$licenses/plutovg.txt"
cp "$root/vendor/plutovg/source/FTL.TXT" "$licenses/plutovg-freetype.txt"
cp "$root/vendor/harfbuzz/COPYING" "$licenses/harfbuzz.txt"
cp "$root/vendor/sheenbidi/LICENSE" "$licenses/sheenbidi.txt"
cp "$root/vendor/abseil-cpp/LICENSE" "$licenses/abseil.txt"
cp "$root/vendor/webrtc-audio-processing/COPYING" "$licenses/webrtc-audio-processing.txt"
sed -n '/^This software is available under 2 licenses/,$p' "$root/vendor/stb/stb_truetype.h" > "$licenses/stb.txt"
cp "$root/vendor/fonts/NotoEmoji-OFL.txt" "$licenses/noto-emoji.txt"
# The npm packages in the archive: those that esbuild bundled (its metafiles), plus p5 and the WebRTC module,
# which are copied as files.
node -e '
const fs = require("fs"), path = require("path")
const [root, target, ...metafiles] = process.argv.slice(1)
const wrtc = fs.realpathSync(path.join(root, "node_modules/@roamhq/wrtc"))
const directories = new Set([fs.realpathSync(path.join(root, "node_modules/p5")), wrtc, path.join(wrtc, "../wrtc-linux-x64")])
for (const metafile of metafiles)
  for (const input of Object.keys(JSON.parse(fs.readFileSync(metafile, "utf8")).inputs)) {
    const match = input.match(/^(.*node_modules\/(?:@[^/]+\/)?[^/]+)\//)
    if (match) directories.add(path.resolve(root, match[1]))
  }
const lines = []
for (const directory of directories) {
  const { name, version, license } = JSON.parse(fs.readFileSync(path.join(directory, "package.json"), "utf8"))
  lines.push(`${name}@${version}\t${license || "see the files"}`)
  const files = fs.readdirSync(directory).filter((file) => /^(licen[cs]e|copying|notice)/i.test(file))
  if (!files.length) continue
  const folder = path.join(target, `${name.replace("/", "+")}@${version}`)
  fs.mkdirSync(folder, { recursive: true })
  for (const file of files) fs.copyFileSync(path.join(directory, file), path.join(folder, file))
}
fs.writeFileSync(path.join(target, "packages.tsv"), [...new Set(lines)].sort().join("\n") + "\n")
' "$root" "$licenses/node" "$build/hydra-bundle.meta.json" "$stage/patch-bay.meta.json"

git -C "$root" archive --format=tar.gz --prefix="hydra-native-$version-source/" \
	-o "$target/source/hydra-native-$version-source.tar.gz" HEAD
cat > "$target/source/README.txt" <<EOF
hydra-native is under the GNU Affero General Public License 3.0 (LICENSE), because it contains hydra-synth
and code of the hydra web editor. hydra-native-$version-source.tar.gz is the source of this build
(commit $(git -C "$root" rev-parse HEAD)).
Build it with \`make\` (README.md); CMakeLists.txt downloads the C libraries of THIRD_PARTY.md at the
pinned versions. hydra-synth, commit 9d29a9f4fd8f9081b9759943f38db36f05b9a88f:
https://github.com/hydra-synth/hydra-synth
The hydra web editor: https://github.com/hydra-synth/hydra
EOF

mkdir -p "$root/dist"
tar -czf "$root/dist/$name.tar.gz" -C "$stage" "$name"
echo "$root/dist/$name.tar.gz"
