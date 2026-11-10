#!/bin/sh
# Packs the release archive of this system from a finished build:
#   Linux x86_64:  dist/hydra-native-VERSION-linux-x86_64.tar.gz
#   macOS arm64:   dist/hydra-native-VERSION-macos-arm64.zip with hydra-native.app
#   Windows x64:   dist/hydra-native-VERSION-windows-x64.zip (run in Git Bash)
# usage: tools/package.sh [VERSION]   (default: git describe; run `make` first)
# HYDRA_NATIVE_BUILD selects a build directory other than build/.
# The macOS and Windows branches are not tested yet: the program does not link there until the ports of
# PORTING.md are complete.
#
# The program finds its files relative to its own path: the bundle next to it, and tools/, vendor/fonts/ and
# assets/ one directory up (src/main.c). bin/ keeps that layout, so the unpacked folder runs from any place. In
# the macOS app, Contents/MacOS is that folder, and Contents/ holds tools/, vendor/ and assets/.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
build=${HYDRA_NATIVE_BUILD:-$root/build}
version=${1:-$(git -C "$root" describe --tags --always --dirty)}
version=${version#v}
case $(uname -s) in
Darwin) system=macos-arm64 wrtc=wrtc-darwin-arm64 ;;
MINGW* | MSYS*) system=windows-x64 wrtc=wrtc-win32-x64 ;;
*) system=linux-x86_64 wrtc=wrtc-linux-x64 ;;
esac
name=hydra-native-$version-$system
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
target=$stage/$name
contents=$target # the folder with the program folder, tools/, vendor/ and assets/
bin=$target/bin
if [ "$system" = macos-arm64 ]; then
	contents=$target/hydra-native.app/Contents
	bin=$contents/MacOS
fi

mkdir -p "$bin" "$contents/tools" "$contents/vendor/fonts" "$contents/assets" "$contents/node_modules/@roamhq" \
	"$target/licenses/node" "$target/source"

case $system in
linux-x86_64)
	cp "$build/hydra-native" "$build/hydra-native-capture" "$build/libhydra-webrtc-apm.so" "$bin/"
	strip --strip-debug "$bin/hydra-native" "$bin/hydra-native-capture" "$bin/libhydra-webrtc-apm.so"
	cp -r "$root/assets/icon" "$root/assets/hydra-native.desktop" "$contents/assets/"
	;;
macos-arm64)
	# ANGLE (its dylibs have @rpath install names) and SDL3 next to the program, which has @executable_path as
	# rpath. The app has only the ad-hoc signature of the linker, so Gatekeeper asks on the first start.
	cp "$build/hydra-native" "$build/hydra-native-capture" "$build/libEGL.dylib" "$build/libGLESv2.dylib" \
		"$build"/libSDL3*.dylib "$bin/"
	mkdir -p "$contents/Resources"
	cp "$root/assets/icon/hydra-native.icns" "$contents/Resources/"
	cat > "$contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>hydra-native</string>
	<key>CFBundleIdentifier</key><string>hydra-native</string>
	<key>CFBundleName</key><string>hydra-native</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>$version</string>
	<key>CFBundleIconFile</key><string>hydra-native.icns</string>
	<key>NSHighResolutionCapable</key><true/>
	<key>NSCameraUsageDescription</key><string>initCam() uses the camera as a video source.</string>
	<key>NSMicrophoneUsageDescription</key><string>The audio input drives a.fft.</string>
</dict>
</plist>
PLIST
	;;
windows-x64)
	cp "$build/hydra-native.exe" "$build/hydra-native-capture.exe" "$build"/*.dll "$bin/"
	;;
esac
cp "$build/hydra-bundle.js" "$build/p5.min.js" "$bin/"
cp "$root/vendor/fonts/NotoEmoji.ttf" "$contents/vendor/fonts/"

# The patch bay helper as one file, so that only the native WebRTC module comes from node_modules.
# Its require() (for wrtc.node) resolves from tools/, like the require of Node.js in the repo.
"$root/node_modules/.bin/esbuild" "$root/tools/patch-bay.mjs" --bundle --platform=node --format=esm \
	--log-level=warning --banner:js="import { createRequire } from 'node:module'; const require = createRequire(import.meta.url);" \
	--outfile="$contents/tools/patch-bay.mjs" --metafile="$stage/patch-bay.meta.json"
# pnpm installs only the prebuilt module of the system that runs it, so each system packs its own.
cp -rL "$(realpath "$root/node_modules/@roamhq/wrtc")/../$wrtc" "$contents/node_modules/@roamhq/"

# Licenses: the AGPL-3.0 of the program, the C libraries in the binary, the font, and the npm packages in the archive.
cp "$root/node_modules/hydra-synth/LICENSE" "$target/LICENSE"
cp "$root/THIRD_PARTY.md" "$target/"
licenses=$target/licenses
cp "$root/vendor/quickjs/LICENSE" "$licenses/quickjs.txt"
cp "$root/vendor/plutovg/LICENSE" "$licenses/plutovg.txt"
cp "$root/vendor/plutovg/source/FTL.TXT" "$licenses/plutovg-freetype.txt"
cp "$root/vendor/harfbuzz/COPYING" "$licenses/harfbuzz.txt"
cp "$root/vendor/sheenbidi/LICENSE" "$licenses/sheenbidi.txt"
if [ "$system" = linux-x86_64 ]; then # the audio processing of the system, through the shim
	cp "$root/vendor/abseil-cpp/LICENSE" "$licenses/abseil.txt"
	cp "$root/vendor/webrtc-audio-processing/COPYING" "$licenses/webrtc-audio-processing.txt"
else # ANGLE and SDL3 in the program folder
	cp "$root/vendor/angle/LICENSE" "$licenses/angle.txt" 2>/dev/null || echo "ANGLE: see https://chromium.googlesource.com/angle/angle/+/refs/heads/main/LICENSE" > "$licenses/angle.txt"
fi
sed -n '/^This software is available under 2 licenses/,$p' "$root/vendor/stb/stb_truetype.h" > "$licenses/stb.txt"
cp "$root/vendor/fonts/NotoEmoji-OFL.txt" "$licenses/noto-emoji.txt"
# The npm packages in the archive: those that esbuild bundled (its metafiles), plus p5 and the WebRTC module,
# which are copied as files.
WRTC=$wrtc node -e '
const fs = require("fs"), path = require("path")
const [root, target, ...metafiles] = process.argv.slice(1)
const wrtc = fs.realpathSync(path.join(root, "node_modules/@roamhq/wrtc"))
const directories = new Set([fs.realpathSync(path.join(root, "node_modules/p5")), wrtc, path.join(wrtc, "../" + process.env.WRTC)])
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
case $system in
linux-x86_64)
	tar -czf "$root/dist/$name.tar.gz" -C "$stage" "$name"
	echo "$root/dist/$name.tar.gz"
	;;
macos-arm64)
	rm -f "$root/dist/$name.zip"
	(cd "$stage" && ditto -c -k --keepParent "$name" "$root/dist/$name.zip") # keeps the app bundle intact
	echo "$root/dist/$name.zip"
	;;
windows-x64)
	rm -f "$root/dist/$name.zip"
	(cd "$stage" && powershell -NoProfile -Command "Compress-Archive -Path '$name' -DestinationPath '$(cygpath -w "$root/dist/$name.zip")'")
	echo "$root/dist/$name.zip"
	;;
esac
