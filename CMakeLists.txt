cmake_minimum_required(VERSION 3.25)
project(hydra-native VERSION 0.1.1 LANGUAGES C)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# The program finds its bundle and helpers next to itself and its icon one directory up, so every
# build directory is a direct child of the source directory (build/, build-clean/).
# The generator expression keeps Visual Studio from adding a Release/ or Debug/ folder.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY $<1:${CMAKE_BINARY_DIR}>)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY $<1:${CMAKE_BINARY_DIR}>)

# ---------------------------------------------------------------- downloaded sources, pinned by hash

set(VENDOR ${CMAKE_SOURCE_DIR}/vendor CACHE PATH "Downloaded sources")
set(QUICKJS_VERSION 2026-06-04)
set(STB_COMMIT 4e9b368bfc071e97206ea5c6adba68dd0cbf8897)
set(LIBREMIDI_VERSION 5.4.3)
set(GITHUB_RAW https://raw.githubusercontent.com)

# Downloads `url` to `path` in up to 3 attempts; sets `result` to TRUE when the file has the hash.
# The hash is checked after the download: with EXPECTED_HASH, a failed download is a fatal error at once.
function(download_verified url path sha256 result)
  foreach(attempt RANGE 1 3)
    message(STATUS "Downloading ${url}")
    file(DOWNLOAD ${url} ${path} STATUS status)
    list(GET status 0 code)
    if(code EQUAL 0)
      file(SHA256 ${path} actual)
      if(actual STREQUAL sha256)
        set(${result} TRUE PARENT_SCOPE)
        return()
      endif()
      set(status "SHA-256 ${actual}, expected ${sha256}")
    endif()
    file(REMOVE ${path})
    message(WARNING "Attempt ${attempt} to download ${url} failed: ${status}")
  endforeach()
  set(${result} FALSE PARENT_SCOPE)
endfunction()

# Extracts the archive at `url` into vendor/<name>, unless vendor/<name>/<marker> is there already.
function(vendor_archive name url sha256 marker)
  if(EXISTS ${VENDOR}/${name}/${marker})
    return()
  endif()
  get_filename_component(archive ${url} NAME)
  set(download ${CMAKE_BINARY_DIR}/downloads/${archive})
  download_verified(${url} ${download} ${sha256} downloaded)
  if(NOT downloaded)
    message(FATAL_ERROR "Cannot download ${url}")
  endif()
  set(extract ${VENDOR}/.extract-${name}) # on the file system of vendor/, for the rename
  file(REMOVE_RECURSE ${extract})
  file(ARCHIVE_EXTRACT INPUT ${download} DESTINATION ${extract})
  file(GLOB top LIST_DIRECTORIES true ${extract}/*)
  list(LENGTH top count)
  file(REMOVE_RECURSE ${VENDOR}/${name})
  if(count EQUAL 1 AND IS_DIRECTORY "${top}")
    file(RENAME ${top} ${VENDOR}/${name})
    file(REMOVE_RECURSE ${extract})
  else() # an archive without one top folder
    file(RENAME ${extract} ${VENDOR}/${name})
  endif()
endfunction()

function(vendor_file path url sha256)
  if(EXISTS ${VENDOR}/${path})
    return()
  endif()
  download_verified(${url} ${VENDOR}/${path} ${sha256} downloaded)
  if(NOT downloaded)
    message(FATAL_ERROR "Cannot download ${url}")
  endif()
endfunction()

vendor_archive(quickjs https://bellard.org/quickjs/quickjs-${QUICKJS_VERSION}.tar.xz
  b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a quickjs.c)
set(STB_stb_image_SHA256 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3)
set(STB_stb_image_write_SHA256 cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05)
set(STB_stb_truetype_SHA256 ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab)
foreach(header stb_image stb_image_write stb_truetype)
  vendor_file(stb/${header}.h ${GITHUB_RAW}/nothings/stb/${STB_COMMIT}/${header}.h ${STB_${header}_SHA256})
endforeach()

# ---------------------------------------------------------------- window and OpenGL ES

# Linux uses SDL3 and the GLES driver of the system (Mesa). macOS and Windows use ANGLE: GLES on Metal and on
# Direct3D 11, as prebuilt libraries of jeremyfa/build-angle, which builds the pinned upstream commit with the
# public GitHub Actions workflow of that repository (see PORTING.md). The program loads them from its folder.
set(ANGLE_RELEASE https://github.com/jeremyfa/build-angle/releases/download/angle-9684da7)
set(SDL3_VERSION 3.4.2)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(HYDRA_LINUX TRUE)
  find_package(SDL3 REQUIRED CONFIG)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLES REQUIRED IMPORTED_TARGET glesv2)
  add_library(gles INTERFACE)
  target_link_libraries(gles INTERFACE PkgConfig::GLES)
else()
  if(APPLE)
    vendor_archive(angle ${ANGLE_RELEASE}/angle-mac-universal.zip
      8049dd6cc27c8e13827785ad3aae89b0709f020ecc230ac9dfeca79833cf186b lib/libGLESv2.dylib)
    set(ANGLE_RUNTIME ${VENDOR}/angle/lib/libEGL.dylib ${VENDOR}/angle/lib/libGLESv2.dylib)
    set(ANGLE_GLES ${VENDOR}/angle/lib/libGLESv2.dylib)
    find_package(SDL3 REQUIRED CONFIG) # Homebrew: brew install sdl3
  elseif(WIN32)
    vendor_archive(angle ${ANGLE_RELEASE}/angle-windows-x64.zip
      b5be74ac235ff45b60c03f6bd1434df1dfbcf82fcc0742e92124a7e35fd30283 lib/libGLESv2.lib)
    file(GLOB ANGLE_RUNTIME ${VENDOR}/angle/bin/*.dll)
    set(ANGLE_GLES ${VENDOR}/angle/lib/libGLESv2.lib)
    vendor_archive(sdl3-vc https://github.com/libsdl-org/SDL/releases/download/release-${SDL3_VERSION}/SDL3-devel-${SDL3_VERSION}-VC.zip
      fd4c60400634df13a86b69432b22b32f23bccd6a2f4c6a34b42b471c65eb4fd2 cmake/SDL3Config.cmake)
    find_package(SDL3 REQUIRED CONFIG PATHS ${VENDOR}/sdl3-vc/cmake NO_DEFAULT_PATH)
    list(APPEND ANGLE_RUNTIME ${VENDOR}/sdl3-vc/lib/x64/SDL3.dll)
  else()
    message(FATAL_ERROR "hydra-native builds on Linux, macOS and Windows")
  endif()
  add_library(gles INTERFACE)
  target_include_directories(gles INTERFACE ${VENDOR}/angle/include)
  target_link_libraries(gles INTERFACE ${ANGLE_GLES})
  # The libraries go next to the programs, where src/window.c and the dynamic linker find them.
  file(COPY ${ANGLE_RUNTIME} DESTINATION ${CMAKE_BINARY_DIR})
  set(CMAKE_BUILD_RPATH "@executable_path")
endif()

# ---------------------------------------------------------------- vendored libraries

if(MSVC)
  set(QUIET /O2 /w) # QuickJS does not build with MSVC; quickjs-ng replaces it there (PORTING.md)
else()
  set(QUIET -O2 -w)
endif()

if(NOT MSVC)
add_library(quickjs STATIC ${VENDOR}/quickjs/quickjs.c ${VENDOR}/quickjs/dtoa.c ${VENDOR}/quickjs/libregexp.c
  ${VENDOR}/quickjs/libunicode.c ${VENDOR}/quickjs/cutils.c ${VENDOR}/quickjs/quickjs-libc.c)
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE CONFIG_VERSION="${QUICKJS_VERSION}")
target_compile_options(quickjs PRIVATE ${QUIET})
target_include_directories(quickjs PUBLIC ${VENDOR}/quickjs)
endif()

if(MSVC)
  set(WARNINGS /W3)
else()
  set(WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

if(WIN32)
  set(PROCESS_SOURCE src/process_windows.c)
else()
  set(PROCESS_SOURCE src/process_posix.c)
endif()

# Opens a hidden window with the GLES context of src/window.c and draws one pass (Mesa, ANGLE on Metal or D3D11).
add_executable(window-test test/window_test.c src/window.c)
target_compile_options(window-test PRIVATE ${WARNINGS})
target_link_libraries(window-test PRIVATE SDL3::SDL3 gles)

# The screen capture of initScreen, initMonitor, initArea and initWindow (src/capture.h), a video source command.
# Backends: ScreenCaptureKit on macOS, Windows.Graphics.Capture (C++/WinRT of the Windows SDK) on Windows, the
# portal and Mutter over GDBus on Linux.
if(APPLE)
  enable_language(OBJC)
  add_executable(hydra-native-capture src/capture_main.c src/capture_macos.m ${PROCESS_SOURCE})
  target_compile_options(hydra-native-capture PRIVATE $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
  target_link_libraries(hydra-native-capture PRIVATE "-framework ScreenCaptureKit" "-framework CoreMedia"
    "-framework CoreVideo" "-framework CoreGraphics" "-framework Accelerate" "-framework AppKit")
elseif(WIN32)
  enable_language(CXX)
  add_executable(hydra-native-capture src/capture_main.c src/capture_windows.cpp ${PROCESS_SOURCE})
  # C++/WinRT of current Visual Studio needs C++20 coroutines; with C++17 it asks for the removed /await.
  set_target_properties(hydra-native-capture PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  target_link_libraries(hydra-native-capture PRIVATE d3d11 dxgi windowsapp)
else()
  pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-unix-2.0)
  add_executable(hydra-native-capture src/capture_main.c src/capture_linux.c)
  target_link_libraries(hydra-native-capture PRIVATE PkgConfig::GIO)
endif()
target_compile_options(hydra-native-capture PRIVATE ${WARNINGS})

# The webRTC patch bay of pb and s0.initStream (src/patch_bay_main.c): webrtcbin of GStreamer, with the signalling
# over a WebSocket of libsoup. Linux only for now (PORTING.md).
if(HYDRA_LINUX)
  pkg_check_modules(PATCH_BAY IMPORTED_TARGET gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0
    gstreamer-app-1.0 libsoup-3.0 json-glib-1.0 gio-unix-2.0)
  if(PATCH_BAY_FOUND)
    add_executable(hydra-native-patch-bay src/patch_bay_main.c)
    target_compile_options(hydra-native-patch-bay PRIVATE ${WARNINGS})
    target_compile_definitions(hydra-native-patch-bay PRIVATE GST_USE_UNSTABLE_API) # the API of webrtcbin
    target_link_libraries(hydra-native-patch-bay PRIVATE PkgConfig::PATCH_BAY)
  else()
    message(STATUS "The patch bay (pb) is not built: GStreamer, libsoup 3 or json-glib development files are missing")
  endif()
endif()

# ---------------------------------------------------------------- audio input and MIDI

# The audio analysis of `a` with the audio input of the system (src/audio_input.h), and MIDI (src/midi.h):
# PulseAudio and the ALSA sequencer on Linux, SDL3 and libremidi on macOS and Windows. On Linux,
# HYDRA_SDL_AUDIO_LIBREMIDI builds the code of macOS and Windows instead, to test it. The MIDI lists of that build
# also show the ALSA ports of libremidi itself, which CoreMIDI and WinMM do not have.
if(HYDRA_LINUX)
  option(HYDRA_SDL_AUDIO_LIBREMIDI "Use SDL3 audio and libremidi as on macOS and Windows" OFF)
  pkg_check_modules(AUDIO_MIDI REQUIRED IMPORTED_TARGET libpulse-simple libpulse alsa)
endif()
add_library(hydra-audio-midi OBJECT src/audio.c src/audio_processing.c)
if(HYDRA_LINUX AND NOT HYDRA_SDL_AUDIO_LIBREMIDI)
  target_sources(hydra-audio-midi PRIVATE src/audio_input_pulse.c src/midi.c)
  target_link_libraries(hydra-audio-midi PUBLIC PkgConfig::AUDIO_MIDI)
else()
  target_sources(hydra-audio-midi PRIVATE src/audio_input_sdl.c)
  enable_language(CXX) # libremidi; the Linux program is C only

  # libremidi in its header-only mode, with only the backend of the system: CoreMIDI, WinMM, or the ALSA
  # sequencer (opened at run time) for the test build of Linux.
  vendor_archive(libremidi https://github.com/celtera/libremidi/archive/refs/tags/v${LIBREMIDI_VERSION}.tar.gz
    a1c369c6aab01c80f1506345d1c099a167b330d1d59159aa7583f7b0b9f966b9 include/libremidi/libremidi.hpp)
  add_library(libremidi INTERFACE)
  target_include_directories(libremidi SYSTEM INTERFACE ${VENDOR}/libremidi/include)
  target_compile_definitions(libremidi INTERFACE LIBREMIDI_HEADER_ONLY LIBREMIDI_NO_BOOST)
  target_compile_features(libremidi INTERFACE cxx_std_20)
  if(APPLE)
    target_compile_definitions(libremidi INTERFACE LIBREMIDI_COREMIDI)
    target_link_libraries(libremidi INTERFACE "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation")
  elseif(WIN32)
    target_compile_definitions(libremidi INTERFACE LIBREMIDI_WINMM)
    target_link_libraries(libremidi INTERFACE winmm)
    # The WinMM code of libremidi uses the wide-character functions; only this file gets UNICODE.
    set_source_files_properties(src/midi_libremidi.cc PROPERTIES COMPILE_DEFINITIONS "UNICODE;_UNICODE")
  else()
    target_compile_definitions(libremidi INTERFACE LIBREMIDI_ALSA)
    target_link_libraries(libremidi INTERFACE ${CMAKE_DL_LIBS})
  endif()
  target_sources(hydra-audio-midi PRIVATE src/midi_libremidi.cc)
  target_link_libraries(hydra-audio-midi PUBLIC libremidi)
endif()
target_compile_options(hydra-audio-midi PRIVATE ${WARNINGS})
target_compile_definitions(hydra-audio-midi PRIVATE $<$<C_COMPILER_ID:MSVC>:_USE_MATH_DEFINES>)
target_link_libraries(hydra-audio-midi PUBLIC SDL3::SDL3 gles)

if(WIN32)
  # No program on Windows yet: QuickJS does not build with MSVC (quickjs-ng replaces it, PORTING.md), and the
  # program still includes POSIX headers. The window test and the capture tool build.
  enable_testing()
  add_test(NAME window COMMAND window-test)
  set_tests_properties(window PROPERTIES LABELS window)
  return()
endif()

# ---------------------------------------------------------------- programs (Linux and macOS)

add_executable(hydra-native src/main.c src/engine.c src/bindings.c src/sandbox.c src/snapshot.c src/code_socket.c
  src/text.c src/video.c src/fetch.c
  src/thumbnails.c src/recorder.c src/patch_bay.c src/editor.c src/ui.c src/settings.c src/overlay.c
  src/keymap.c src/programs.c src/signature.c src/window.c ${PROCESS_SOURCE})
target_compile_options(hydra-native PRIVATE ${WARNINGS})
target_compile_definitions(hydra-native PRIVATE HYDRA_NATIVE_VERSION="${PROJECT_VERSION}")
target_include_directories(hydra-native PRIVATE ${VENDOR}/stb)
target_link_libraries(hydra-native PRIVATE quickjs hydra-audio-midi SDL3::SDL3 gles
  m dl pthread)
if(TARGET libremidi) # C++, so the C++ driver links the program
  set_target_properties(hydra-native PROPERTIES LINKER_LANGUAGE CXX)
endif()

# Starts the shader compile of src/programs.c next to a hidden window and compiles two programs.
add_executable(programs-test test/programs_test.c src/programs.c src/window.c)
target_compile_options(programs-test PRIVATE ${WARNINGS})
target_link_libraries(programs-test PRIVATE SDL3::SDL3 gles pthread)

# ---------------------------------------------------------------- JavaScript

find_program(PNPM pnpm REQUIRED)
set(NODE_MODULES ${CMAKE_SOURCE_DIR}/node_modules)
add_custom_command(OUTPUT ${NODE_MODULES}/.modules.yaml
  COMMAND ${PNPM} install --frozen-lockfile
  DEPENDS ${CMAKE_SOURCE_DIR}/package.json ${CMAKE_SOURCE_DIR}/pnpm-lock.yaml
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
file(GLOB JS_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/js/*.js ${CMAKE_SOURCE_DIR}/js/hydra-editor/*)
# The metafile lists the bundled npm packages for the license files of the release archive.
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/hydra-bundle.js ${CMAKE_BINARY_DIR}/hydra-bundle.meta.json
  COMMAND ${NODE_MODULES}/.bin/esbuild js/main.js --bundle --format=iife --log-level=warning --inject:js/native.js
    --outfile=${CMAKE_BINARY_DIR}/hydra-bundle.js --metafile=${CMAKE_BINARY_DIR}/hydra-bundle.meta.json
    --alias:regl=./js/regl-shim.js --alias:raf-loop=./js/raf-loop-shim.js --alias:meyda=./js/meyda-shim.js
  DEPENDS ${JS_SOURCES} ${NODE_MODULES}/.modules.yaml
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_custom_target(javascript ALL DEPENDS ${CMAKE_BINARY_DIR}/hydra-bundle.js)

# ---------------------------------------------------------------- tests

enable_testing()
set(SANITIZERS -fsanitize=address,undefined)

add_executable(editor-test EXCLUDE_FROM_ALL test/editor_test.c src/editor.c)
add_executable(ui-test EXCLUDE_FROM_ALL test/ui_test.c src/ui.c src/keymap.c src/editor.c src/settings.c
  src/signature.c)
add_executable(audio-test EXCLUDE_FROM_ALL test/audio_test.c src/audio_processing.c)
add_executable(process-test EXCLUDE_FROM_ALL test/process_test.c ${PROCESS_SOURCE})
target_link_libraries(ui-test PRIVATE quickjs SDL3::SDL3 m dl pthread)
target_link_libraries(audio-test PRIVATE m)
set(UNIT_TESTS editor-test ui-test audio-test process-test)
if(HYDRA_LINUX)
# midi-test sends through the ALSA sequencer; with HYDRA_SDL_AUDIO_LIBREMIDI it tests src/midi_libremidi.cc.
add_executable(midi-test EXCLUDE_FROM_ALL test/midi_test.c)
if(HYDRA_SDL_AUDIO_LIBREMIDI)
  target_sources(midi-test PRIVATE src/midi_libremidi.cc)
  target_link_libraries(midi-test PRIVATE libremidi)
  set_target_properties(midi-test PROPERTIES LINKER_LANGUAGE CXX)
else()
  target_sources(midi-test PRIVATE src/midi.c)
endif()
target_link_libraries(midi-test PRIVATE PkgConfig::AUDIO_MIDI m)
list(APPEND UNIT_TESTS midi-test)
endif()
foreach(test ${UNIT_TESTS})
  target_compile_options(${test} PRIVATE -O1 ${WARNINGS} ${SANITIZERS})
  target_link_options(${test} PRIVATE ${SANITIZERS})
  add_test(NAME ${test} COMMAND ${test})
  set_tests_properties(${test} PROPERTIES LABELS unit)
endforeach()
add_custom_target(unit-tests DEPENDS ${UNIT_TESTS} hydra-native-capture)
add_test(NAME mutator COMMAND node ${CMAKE_SOURCE_DIR}/test/mutator_test.mjs)
add_test(NAME declarations COMMAND node ${CMAKE_SOURCE_DIR}/test/declarations_test.mjs)
set_tests_properties(mutator declarations PROPERTIES LABELS unit)
if(HYDRA_LINUX)
  add_test(NAME portal COMMAND python3 ${CMAKE_SOURCE_DIR}/test/portal_test.py)
  set_tests_properties(portal PROPERTIES LABELS unit)
  add_test(NAME portal-stop COMMAND python3 ${CMAKE_SOURCE_DIR}/test/portal_stop_test.py)
  set_tests_properties(portal-stop PROPERTIES LABELS unit)
endif()

# The checks run the program in hidden windows, with a settings folder of their own. Their sketches use the global
# `native` (HYDRA_NATIVE_TEST, js/native.js); test/sandbox_test.py removes it again.
add_test(NAME check COMMAND hydra-native --hidden ${CMAKE_SOURCE_DIR}/test/check.js)
add_test(NAME window COMMAND window-test)
add_test(NAME programs COMMAND programs-test)
foreach(check fetch gallery patch_bay error sandbox)
  add_test(NAME ${check} COMMAND python3 ${CMAKE_SOURCE_DIR}/test/${check}_test.py)
endforeach()
set_tests_properties(check window programs fetch gallery patch_bay error sandbox PROPERTIES LABELS check RUN_SERIAL true)
get_property(ALL_TESTS DIRECTORY PROPERTY TESTS)
set_tests_properties(${ALL_TESTS} PROPERTIES TIMEOUT 300 # seconds; a sketch that never exits stops CI otherwise
  ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config;HYDRA_NATIVE_BUILD=${CMAKE_BINARY_DIR};HYDRA_NATIVE_TEST=1")
