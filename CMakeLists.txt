cmake_minimum_required(VERSION 3.25)
project(hydra-native VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# The program finds its bundle next to itself and its tools, fonts and icon one directory up, so every
# build directory is a direct child of the source directory (build/, build-clean/).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ---------------------------------------------------------------- downloaded sources, pinned by hash

set(VENDOR ${CMAKE_SOURCE_DIR}/vendor CACHE PATH "Downloaded sources; the program finds its emoji font in vendor/fonts")
set(QUICKJS_VERSION 2026-06-04)
set(PLUTOVG_VERSION 1.3.3)
set(HARFBUZZ_VERSION 14.6.0)
set(SHEENBIDI_VERSION 2.9.0)
set(WEBRTC_APM_VERSION 1.3)
set(ABSEIL_VERSION 20230125.1)
set(STB_COMMIT 4e9b368bfc071e97206ea5c6adba68dd0cbf8897)
set(EMOJI_FONT_COMMIT 8b0a1d0f5983c89bc2b93f1b5fb55f9e252744b5)
set(GITHUB_RAW https://raw.githubusercontent.com)

# Extracts the archive at `url` into vendor/<name>, unless vendor/<name>/<marker> is there already.
function(vendor_archive name url sha256 marker)
  if(EXISTS ${VENDOR}/${name}/${marker})
    return()
  endif()
  get_filename_component(archive ${url} NAME)
  set(download ${CMAKE_BINARY_DIR}/downloads/${archive})
  message(STATUS "Downloading ${url}")
  file(DOWNLOAD ${url} ${download} EXPECTED_HASH SHA256=${sha256} STATUS status)
  list(GET status 0 code)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "Cannot download ${url}: ${status}")
  endif()
  set(extract ${VENDOR}/.extract-${name}) # on the file system of vendor/, for the rename
  file(REMOVE_RECURSE ${extract})
  file(ARCHIVE_EXTRACT INPUT ${download} DESTINATION ${extract})
  file(GLOB top LIST_DIRECTORIES true ${extract}/*)
  file(REMOVE_RECURSE ${VENDOR}/${name})
  file(RENAME ${top} ${VENDOR}/${name})
  file(REMOVE_RECURSE ${extract})
endfunction()

# Downloads `url` to vendor/<path>, unless it is there already.
function(vendor_file path url sha256)
  if(EXISTS ${VENDOR}/${path})
    return()
  endif()
  message(STATUS "Downloading ${url}")
  file(DOWNLOAD ${url} ${VENDOR}/${path} EXPECTED_HASH SHA256=${sha256} STATUS status)
  list(GET status 0 code)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "Cannot download ${url}: ${status}")
  endif()
endfunction()

vendor_archive(quickjs https://bellard.org/quickjs/quickjs-${QUICKJS_VERSION}.tar.xz
  b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a quickjs.c)
vendor_archive(plutovg https://github.com/sammycage/plutovg/archive/refs/tags/v${PLUTOVG_VERSION}.tar.gz
  2b0d17a6e016f47b86f9c00e2cb82600041b1ea1f7d2a00c2d46ae542cbfed3c include/plutovg.h)
vendor_archive(harfbuzz
  https://github.com/harfbuzz/harfbuzz/releases/download/${HARFBUZZ_VERSION}/harfbuzz-${HARFBUZZ_VERSION}.tar.xz
  d07a007327277708a2a73ae437887cdbaf282937f6d03ca5467723e9099af586 src/harfbuzz.cc)
vendor_archive(sheenbidi https://github.com/Tehreer/SheenBidi/archive/refs/tags/v${SHEENBIDI_VERSION}.tar.gz
  e90ae142c6fc8b94366f3526f84b349a2c10137f87093db402fe51f6eace6d13 Source/SheenBidi.c)
set(STB_stb_image_SHA256 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3)
set(STB_stb_image_write_SHA256 cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05)
set(STB_stb_truetype_SHA256 ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab)
foreach(header stb_image stb_image_write stb_truetype)
  vendor_file(stb/${header}.h ${GITHUB_RAW}/nothings/stb/${STB_COMMIT}/${header}.h ${STB_${header}_SHA256})
endforeach()
set(EMOJI_FONT_URL ${GITHUB_RAW}/google/fonts/${EMOJI_FONT_COMMIT}/ofl/notoemoji)
vendor_file(fonts/NotoEmoji.ttf ${EMOJI_FONT_URL}/NotoEmoji%5Bwght%5D.ttf de6c18832938afc99caf132b39d6a30a19bac7f2e812e28db2535b4608d27551)
vendor_file(fonts/NotoEmoji-OFL.txt ${EMOJI_FONT_URL}/OFL.txt 500bb1ccf43df7bbb522112f9133a52b16e1c35e809632f5d8609b179152de5b)

# ---------------------------------------------------------------- system libraries

find_package(SDL3 REQUIRED CONFIG)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEM REQUIRED IMPORTED_TARGET glesv2 libpulse-simple libpulse alsa)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-unix-2.0)
find_library(WEBRTC_APM_LIBRARY NAMES libwebrtc-audio-processing-1.so.3)

# ---------------------------------------------------------------- vendored libraries

add_library(quickjs STATIC ${VENDOR}/quickjs/quickjs.c ${VENDOR}/quickjs/dtoa.c ${VENDOR}/quickjs/libregexp.c
  ${VENDOR}/quickjs/libunicode.c ${VENDOR}/quickjs/cutils.c ${VENDOR}/quickjs/quickjs-libc.c)
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE CONFIG_VERSION="${QUICKJS_VERSION}")
target_compile_options(quickjs PRIVATE -O2 -w)
target_include_directories(quickjs PUBLIC ${VENDOR}/quickjs)

add_library(plutovg STATIC)
foreach(source blend canvas font ft-math ft-raster ft-stroker matrix paint path rasterize surface)
  target_sources(plutovg PRIVATE ${VENDOR}/plutovg/source/plutovg-${source}.c)
endforeach()
target_compile_definitions(plutovg PRIVATE PLUTOVG_BUILD PLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD
  PUBLIC PLUTOVG_BUILD_STATIC)
target_compile_options(plutovg PRIVATE -O2 -w)
target_include_directories(plutovg PUBLIC ${VENDOR}/plutovg/include)

add_library(harfbuzz STATIC ${VENDOR}/harfbuzz/src/harfbuzz.cc)
target_compile_options(harfbuzz PRIVATE -O2 -w -fno-exceptions -fno-rtti)
target_include_directories(harfbuzz PUBLIC ${VENDOR}/harfbuzz/src)

add_library(sheenbidi STATIC ${VENDOR}/sheenbidi/Source/SheenBidi.c)
target_compile_definitions(sheenbidi PRIVATE SB_CONFIG_UNITY)
target_compile_options(sheenbidi PRIVATE -O2 -w)
target_include_directories(sheenbidi PUBLIC ${VENDOR}/sheenbidi/Headers)

set(WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

# ---------------------------------------------------------------- programs

add_executable(hydra-native src/main.c src/canvas2d.c src/svg_filter.c src/webgl.c src/video.c src/audio.c
  src/audio_processing.c src/analog_agc.c src/fetch.c src/thumbnails.c src/midi.c src/recorder.c src/patch_bay.c
  src/editor.c src/bidi.c src/ui.c src/settings.c src/overlay.c src/keymap.c src/programs.c)
target_compile_options(hydra-native PRIVATE ${WARNINGS})
target_compile_definitions(hydra-native PRIVATE HYDRA_NATIVE_VERSION="${PROJECT_VERSION}")
target_include_directories(hydra-native PRIVATE ${VENDOR}/stb)
target_link_libraries(hydra-native PRIVATE quickjs plutovg harfbuzz sheenbidi SDL3::SDL3 PkgConfig::SYSTEM m dl
  pthread)
# harfbuzz is C++, so the C++ driver links the program.
set_target_properties(hydra-native PROPERTIES LINKER_LANGUAGE CXX)

# The screen capture of initScreen, initMonitor, initArea and initWindow (src/capture.h), a video source command.
add_executable(hydra-native-capture src/capture_main.c src/capture_linux.c)
target_compile_options(hydra-native-capture PRIVATE ${WARNINGS})
target_link_libraries(hydra-native-capture PRIVATE PkgConfig::GIO)

# The audio processing of Chrome from the system (not downloaded); only its headers come from upstream.
# C++17 makes absl::optional the std::optional that the system library uses.
if(WEBRTC_APM_LIBRARY)
  vendor_archive(webrtc-audio-processing
    https://freedesktop.org/software/pulseaudio/webrtc-audio-processing/webrtc-audio-processing-${WEBRTC_APM_VERSION}.tar.xz
    2365e93e778d7b61b5d6e02d21c47d97222e9c7deff9e1d0838ad6ec2e86f1b9 webrtc/modules/audio_processing/include/audio_processing.h)
  vendor_archive(abseil-cpp https://github.com/abseil/abseil-cpp/archive/${ABSEIL_VERSION}.tar.gz
    81311c17599b3712069ded20cca09a62ab0bf2a89dfa16993786c8782b7ed145 absl/types/optional.h)
  add_library(hydra-webrtc-apm SHARED src/webrtc_apm.cc)
  set_target_properties(hydra-webrtc-apm PROPERTIES CXX_STANDARD 17)
  target_compile_options(hydra-webrtc-apm PRIVATE -O2 -w)
  target_compile_definitions(hydra-webrtc-apm PRIVATE WEBRTC_POSIX WEBRTC_LINUX)
  target_include_directories(hydra-webrtc-apm PRIVATE ${VENDOR}/webrtc-audio-processing/webrtc ${VENDOR}/abseil-cpp)
  target_link_libraries(hydra-webrtc-apm PRIVATE ${WEBRTC_APM_LIBRARY})
else()
  message(STATUS "libwebrtc-audio-processing-1 is not installed: the audio input has no echo and noise filters")
endif()

# ---------------------------------------------------------------- JavaScript

find_program(PNPM pnpm REQUIRED)
set(NODE_MODULES ${CMAKE_SOURCE_DIR}/node_modules)
add_custom_command(OUTPUT ${NODE_MODULES}/.modules.yaml
  COMMAND ${PNPM} install --frozen-lockfile
  DEPENDS ${CMAKE_SOURCE_DIR}/package.json ${CMAKE_SOURCE_DIR}/pnpm-lock.yaml
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
file(GLOB JS_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/js/*.js ${CMAKE_SOURCE_DIR}/js/hydra-editor/*)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/hydra-bundle.js
  COMMAND ${NODE_MODULES}/.bin/esbuild js/main.js --bundle --format=iife --log-level=warning
    --outfile=${CMAKE_BINARY_DIR}/hydra-bundle.js
    --alias:regl=./js/regl-shim.js --alias:raf-loop=./js/raf-loop-shim.js --alias:meyda=./js/meyda-shim.js
  DEPENDS ${JS_SOURCES} ${NODE_MODULES}/.modules.yaml
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/p5.min.js
  COMMAND ${CMAKE_COMMAND} -E copy ${NODE_MODULES}/p5/lib/p5.min.js ${CMAKE_BINARY_DIR}/p5.min.js
  DEPENDS ${NODE_MODULES}/.modules.yaml)
add_custom_target(javascript ALL DEPENDS ${CMAKE_BINARY_DIR}/hydra-bundle.js ${CMAKE_BINARY_DIR}/p5.min.js)

# ---------------------------------------------------------------- tests

enable_testing()
set(SANITIZERS -fsanitize=address,undefined)

add_executable(editor-test EXCLUDE_FROM_ALL test/editor_test.c src/editor.c src/bidi.c)
add_executable(ui-test EXCLUDE_FROM_ALL test/ui_test.c src/ui.c src/keymap.c src/editor.c src/bidi.c src/settings.c)
add_executable(audio-test EXCLUDE_FROM_ALL test/audio_test.c src/audio_processing.c src/analog_agc.c)
add_executable(midi-test EXCLUDE_FROM_ALL test/midi_test.c src/midi.c)
target_link_libraries(editor-test PRIVATE sheenbidi)
target_link_libraries(ui-test PRIVATE sheenbidi quickjs SDL3::SDL3 m dl pthread)
target_link_libraries(audio-test PRIVATE m dl pthread)
target_link_libraries(midi-test PRIVATE PkgConfig::SYSTEM m pthread)
set(UNIT_TESTS editor-test ui-test audio-test midi-test)
foreach(test ${UNIT_TESTS})
  target_compile_options(${test} PRIVATE -O1 ${WARNINGS} ${SANITIZERS})
  target_link_options(${test} PRIVATE ${SANITIZERS})
  add_test(NAME ${test} COMMAND ${test})
  set_tests_properties(${test} PROPERTIES LABELS unit)
endforeach()
add_custom_target(unit-tests DEPENDS ${UNIT_TESTS} hydra-native-capture)
add_test(NAME portal COMMAND python3 ${CMAKE_SOURCE_DIR}/test/portal_test.py)
set_tests_properties(portal PROPERTIES LABELS unit)

# The checks run the program in hidden windows, with a settings folder of their own.
add_test(NAME check COMMAND hydra-native --hidden --no-vsync --no-analog-agc ${CMAKE_SOURCE_DIR}/test/check.js)
foreach(check fetch gallery patch_bay)
  add_test(NAME ${check} COMMAND python3 ${CMAKE_SOURCE_DIR}/test/${check}_test.py)
endforeach()
set_tests_properties(check fetch gallery patch_bay PROPERTIES LABELS check RUN_SERIAL true)
get_property(ALL_TESTS DIRECTORY PROPERTY TESTS)
set_tests_properties(${ALL_TESTS} PROPERTIES
  ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config;HYDRA_NATIVE_BUILD=${CMAKE_BINARY_DIR}")
