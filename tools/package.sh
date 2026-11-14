#!/bin/sh
# Packs the release archive of this system from a finished build:
#   Linux x86_64:  dist/hydra-native-VERSION-linux-x86_64.tar.gz
#   macOS arm64:   dist/hydra-native-VERSION-macos-arm64.zip with hydra-native.app
#   Windows x64:   dist/hydra-native-VERSION-windows-x64.zip (run in Git Bash)
# usage: tools/package.sh [VERSION]   (default: git describe; run `make` first)
# HYDRA_NATIVE_BUILD selects a build directory other than build/.
# CI runs all three branches. The Windows branch needs the llvm-mingw of the build in PATH for its licenses.
#
# The program finds its files relative to its own path: the bundle and the helpers next to it, and assets/ one
# directory up (src/main.c). bin/ keeps that layout, so the unpacked folder runs from any place. In the macOS
# app, Contents/MacOS is that folder, and Contents/ holds assets/.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
build=${HYDRA_NATIVE_BUILD:-$root/build}
version=${1:-$(git -C "$root" describe --tags --always --dirty)}
version=${version#v}
case $(uname -s) in
Darwin) system=macos-arm64 ;;
MINGW* | MSYS*) system=windows-x64 ;;
*) system=linux-x86_64 ;;
esac
name=hydra-native-$version-$system
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
target=$stage/$name
contents=$target # the folder with the program folder and assets/
bin=$target/bin
if [ "$system" = macos-arm64 ]; then
	contents=$target/hydra-native.app/Contents
	bin=$contents/MacOS
fi

mkdir -p "$bin" "$contents/assets" "$target/licenses/node" "$target/source"

case $system in
linux-x86_64)
	cp "$build/hydra-native" "$build/hydra-native-capture" "$build/hydra-native-patch-bay" "$bin/"
	strip --strip-debug "$bin/hydra-native" "$bin/hydra-native-capture" "$bin/hydra-native-patch-bay"
	cp -r "$root/assets/icon" "$root/assets/hydra-native.desktop" "$contents/assets/"
	;;
macos-arm64)
	# ANGLE (its dylibs have @rpath install names) and the SDL3 of Homebrew next to the program, which has
	# @executable_path as rpath. The changed files get a new ad-hoc signature; Gatekeeper asks on the first start.
	cp "$build/hydra-native" "$build/hydra-native-capture" "$build/hydra-native-patch-bay" "$build/libEGL.dylib" \
		"$build/libGLESv2.dylib" "$bin/"
	sdl=$(otool -L "$bin/hydra-native" | awk '/libSDL3/ { print $1 }')
	cp "$sdl" "$bin/"
	install_name_tool -id "@rpath/$(basename "$sdl")" "$bin/$(basename "$sdl")"
	install_name_tool -change "$sdl" "@rpath/$(basename "$sdl")" "$bin/hydra-native"
	strip "$bin/hydra-native-patch-bay"
	codesign --force --sign - "$bin"/*.dylib "$bin/hydra-native" "$bin/hydra-native-capture" "$bin/hydra-native-patch-bay"
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
	# ANGLE with its d3dcompiler_47.dll, and SDL3; the programs link the C++ runtime and winpthreads statically.
	cp "$build/hydra-native.exe" "$build/hydra-native-capture.exe" "$build/hydra-native-patch-bay.exe" \
		"$build"/*.dll "$bin/"
	strip "$bin"/*.exe
	cp -r "$root/assets/icon" "$contents/assets/" # the window icon (src/main.c)
	;;
esac
cp "$build/hydra-bundle.js" "$bin/"

# Licenses: the AGPL-3.0 of the program, the C libraries in the binary, the font, and the npm packages in the archive.
cp "$root/node_modules/hydra-synth/LICENSE" "$target/LICENSE"
cp "$root/THIRD_PARTY.md" "$target/"
licenses=$target/licenses
cp "$root/vendor/quickjs/LICENSE" "$licenses/quickjs.txt"
# The texts of code that is ported or generated into src/ (THIRD_PARTY.md), kept in the repository.
cp "$root/licenses/unicode.txt" "$root/licenses/codemirror.txt" "$licenses/"
if [ "$system" != linux-x86_64 ]; then # libremidi in the program, ANGLE and SDL3 in the program folder (their archives hold no license file)
	cp "$root/vendor/libremidi/LICENSE.md" "$licenses/libremidi.txt"
	cp "$root/licenses/angle.txt" "$root/licenses/sdl3.txt" "$licenses/"
fi
sed -n '/^This software is available under 2 licenses/,$p' "$root/vendor/stb/stb_truetype.h" > "$licenses/stb.txt"
# The libraries of hydra-native-patch-bay.
deps=$root/vendor/libdatachannel/deps
cp "$root/vendor/libdatachannel/LICENSE" "$licenses/libdatachannel.txt"
cp "$deps/libjuice/LICENSE" "$licenses/libjuice.txt"
cp "$deps/usrsctp/LICENSE.md" "$licenses/usrsctp.txt"
cp "$deps/libsrtp/LICENSE" "$licenses/libsrtp.txt"
cp "$deps/plog/LICENSE" "$licenses/plog.txt"
cp "$root/vendor/mbedtls/LICENSE" "$licenses/mbedtls.txt"
sed -n '1,/^\*\//p' "$root/vendor/cjson/cJSON.h" > "$licenses/cjson.txt"
if [ "$system" = windows-x64 ]; then # C++/WinRT in the capture tool, and the runtime of llvm-mingw in all programs
	cp "$root/vendor/cppwinrt/LICENSE" "$licenses/cppwinrt.txt"
	toolchain=$(dirname "$(dirname "$(command -v clang)")")
	cp "$toolchain/LICENSE.TXT" "$licenses/llvm.txt"
	cp "$toolchain/x86_64-w64-mingw32/share/mingw32/COPYING.MinGW-w64-runtime.txt" "$licenses/mingw-w64-runtime.txt"
	cp "$toolchain/x86_64-w64-mingw32/share/mingw32/COPYING.winpthreads.txt" "$licenses/winpthreads.txt"
fi
# The npm packages in the archive: those that esbuild bundled (its metafile).
node -e '
const fs = require("fs"), path = require("path")
const [root, target, metafile] = process.argv.slice(1)
const directories = new Set()
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
' "$root" "$licenses/node" "$build/hydra-bundle.meta.json"

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
	# The bsdtar of Windows; Compress-Archive of Windows PowerShell writes backslashes into the paths of the zip.
	(cd "$stage" && "$(cygpath -u "$SYSTEMROOT")/System32/tar.exe" -a -cf "$(cygpath -w "$root/dist/$name.zip")" "$name")
	echo "$root/dist/$name.zip"
	;;
esac
