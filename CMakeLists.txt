cmake_minimum_required(VERSION 3.25)
project(hydra-native VERSION 0.1.3 LANGUAGES C)

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
set(LIBDATACHANNEL_VERSION 0.24.6)
set(MBEDTLS_VERSION 3.6.7)
set(CJSON_COMMIT c859b25da02955fef659d658b8f324b5cde87be3) # v1.7.19
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
  string(REPLACE / - folder ${name}) # a name such as libdatachannel/deps/plog
  set(extract ${VENDOR}/.extract-${folder}) # on the file system of vendor/, for the rename
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
    set(SDL3_RELEASE https://github.com/libsdl-org/SDL/releases/download/release-${SDL3_VERSION})
    set(SDL3_MINGW ${VENDOR}/sdl3-mingw/x86_64-w64-mingw32)
    vendor_archive(sdl3-mingw ${SDL3_RELEASE}/SDL3-devel-${SDL3_VERSION}-mingw.tar.gz
      7b8f0c5453ea68e7afc6bacb79c44b8492aa312fce712fb3be5a7b3ada393cb2
      x86_64-w64-mingw32/lib/cmake/SDL3/SDL3Config.cmake)
    find_package(SDL3 REQUIRED CONFIG PATHS ${SDL3_MINGW}/lib/cmake/SDL3 NO_DEFAULT_PATH)
    list(APPEND ANGLE_RUNTIME ${SDL3_MINGW}/bin/SDL3.dll)
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

# Windows builds with the MinGW-w64 toolchain llvm-mingw (clang and libc++), as QuickJS does not build with
# MSVC. The programs link the C++ runtime and winpthreads statically, so they need no DLL of the toolchain.
if(MSVC)
  message(FATAL_ERROR "Build on Windows with llvm-mingw (README.md); QuickJS does not build with MSVC")
endif()
if(WIN32)
  add_link_options(-static)
endif()

add_library(quickjs STATIC ${VENDOR}/quickjs/quickjs.c ${VENDOR}/quickjs/dtoa.c ${VENDOR}/quickjs/libregexp.c
  ${VENDOR}/quickjs/libunicode.c ${VENDOR}/quickjs/cutils.c ${VENDOR}/quickjs/quickjs-libc.c)
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE CONFIG_VERSION="${QUICKJS_VERSION}")
target_compile_options(quickjs PRIVATE -O2 -w)
target_include_directories(quickjs PUBLIC ${VENDOR}/quickjs)

set(WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

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
  # The headers of C++/WinRT come from its generator (NuGet package Microsoft.Windows.CppWinRT), which reads the
  # metadata of the Windows that runs the build. They need C++20 for their coroutines.
  set(CPPWINRT_VERSION 2.0.250303.1)
  vendor_archive(cppwinrt https://www.nuget.org/api/v2/package/Microsoft.Windows.CppWinRT/${CPPWINRT_VERSION}
    955e3051b35db1c00177ed14ab6b7b995c3b32a53fdb6c184ea53b1dba66e439 bin/cppwinrt.exe)
  set(CPPWINRT_HEADERS ${CMAKE_BINARY_DIR}/cppwinrt)
  if(NOT EXISTS ${CPPWINRT_HEADERS}/winrt/base.h)
    execute_process(COMMAND ${VENDOR}/cppwinrt/bin/cppwinrt.exe -input local -output ${CPPWINRT_HEADERS}
      COMMAND_ERROR_IS_FATAL ANY)
  endif()
  add_executable(hydra-native-capture src/capture_main.c src/capture_windows.cpp ${PROCESS_SOURCE})
  target_include_directories(hydra-native-capture PRIVATE ${CPPWINRT_HEADERS})
  set_target_properties(hydra-native-capture PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  target_link_libraries(hydra-native-capture PRIVATE d3d11 dxgi windowsapp)
else()
  pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-unix-2.0)
  add_executable(hydra-native-capture src/capture_main.c src/capture_linux.c)
  target_link_libraries(hydra-native-capture PRIVATE PkgConfig::GIO)
endif()
target_compile_options(hydra-native-capture PRIVATE ${WARNINGS})

# ---------------------------------------------------------------- WebRTC

# libdatachannel with its submodules at the commits of its release, each a pinned archive, and mbedTLS for TLS,
# DTLS and SRTP, so that no system library is needed. All are static and C++ or C; the helper itself is C.
enable_language(CXX)
set(GITHUB https://github.com)
vendor_archive(libdatachannel
  ${GITHUB}/paullouisageneau/libdatachannel/archive/refs/tags/v${LIBDATACHANNEL_VERSION}.tar.gz
  22a66aebebd8dae561d8e5181da96f2fcc203e067c5b2186c07b39af913464c0 include/rtc/rtc.h)
vendor_archive(libdatachannel/deps/libjuice
  ${GITHUB}/paullouisageneau/libjuice/archive/b89c792e3612faf2f12cf35bcc56857313a06be3.tar.gz
  713b22174e8cb2e06538fa9bac2650bda21b83c86f5128813ab4dcf8f5bea9ed CMakeLists.txt)
vendor_archive(libdatachannel/deps/usrsctp
  ${GITHUB}/paullouisageneau/usrsctp/archive/fec583d54493f879d2ae44a743423bf8a04371ab.tar.gz
  e5c114afe73c9a0ec419fab5f5b3f63f3ce57b09b90d06ea85e63dca8aed3e7c CMakeLists.txt)
vendor_archive(libdatachannel/deps/libsrtp
  ${GITHUB}/cisco/libsrtp/archive/24b3bf8f19b6f5ab4cd2bcceb4f4064efca86fd5.tar.gz
  063478e368d7cd13d04a908d152a46f90bfa728c4b324be6d413b0b92728207c CMakeLists.txt)
vendor_archive(libdatachannel/deps/plog
  ${GITHUB}/SergiusTheBest/plog/archive/94899e0b926ac1b0f4750bfbd495167b4a6ae9ef.tar.gz
  92a08bce559b5f28aa88d3fd9071567414b9f43a83fe2a05a6dd14f1da536072 CMakeLists.txt)
vendor_archive(mbedtls
  ${GITHUB}/Mbed-TLS/mbedtls/releases/download/mbedtls-${MBEDTLS_VERSION}/mbedtls-${MBEDTLS_VERSION}.tar.bz2
  a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6 include/mbedtls/ssl.h)
vendor_file(cjson/cJSON.c ${GITHUB_RAW}/DaveGamble/cJSON/${CJSON_COMMIT}/cJSON.c
  298581a04a36c0165da4b0aade235c23088cb2faa58651d720ea2f3706ed0b0d)
vendor_file(cjson/cJSON.h ${GITHUB_RAW}/DaveGamble/cJSON/${CJSON_COMMIT}/cJSON.h
  25b0145150d500498e4d209cec69c18c42cf818bffcc54690be3b895a2a16dee)

# WebRTC needs the DTLS-SRTP extension, which the default configuration of mbedTLS leaves out.
file(WRITE ${CMAKE_BINARY_DIR}/mbedtls_user_config.h "#define MBEDTLS_SSL_DTLS_SRTP\n")
set(MBEDTLS_USER_CONFIG_FILE ${CMAKE_BINARY_DIR}/mbedtls_user_config.h CACHE FILEPATH "")
set(ENABLE_PROGRAMS OFF CACHE BOOL "")
set(ENABLE_TESTING OFF CACHE BOOL "")
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "")
# The export set of libsrtp needs mbedTLS in an export set too; hydra-native installs neither.
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL OFF CACHE BOOL "")
set(BUILD_SHARED_LIBS OFF) # for the third-party parts of mbedTLS; libdatachannel sets its own
add_subdirectory(${VENDOR}/mbedtls ${CMAKE_BINARY_DIR}/mbedtls EXCLUDE_FROM_ALL)
unset(BUILD_SHARED_LIBS)
add_library(MbedTLS::MbedTLS ALIAS mbedtls)
# The find module of libdatachannel, which libsrtp also uses, takes these results instead of an installed copy.
set(MbedTLS_INCLUDE_DIR ${VENDOR}/mbedtls/include CACHE PATH "")
set(MbedTLS_LIBRARY mbedtls CACHE STRING "")
set(MbedCrypto_LIBRARY mbedcrypto CACHE STRING "") # skips a symbol check that the module cannot run
set(MbedTLS_INCLUDES_X509 TRUE CACHE BOOL "")
set(MbedTLS_INCLUDES_CRYPTO TRUE CACHE BOOL "")
set(MBEDTLS_LIBRARIES mbedtls CACHE STRING "")

set(USE_MBEDTLS ON CACHE BOOL "")
set(NO_EXAMPLES ON CACHE BOOL "")
set(NO_TESTS ON CACHE BOOL "")
set(LIBSRTP_TEST_APPS OFF CACHE BOOL "")
set(ENABLE_WARNINGS_AS_ERRORS OFF CACHE BOOL "")
add_subdirectory(${VENDOR}/libdatachannel ${CMAKE_BINARY_DIR}/libdatachannel EXCLUDE_FROM_ALL)
if(WIN32) # src/configuration.cpp uses std::transform without <algorithm>, which libc++ does not include for it
  target_compile_options(datachannel-static PRIVATE $<$<COMPILE_LANGUAGE:CXX>:SHELL:-include algorithm>)
endif()

# The webRTC patch bay of pb and s0.initStream (src/patch_bay_main.c), with ffmpeg as the video codec at run time.
add_executable(hydra-native-patch-bay src/patch_bay_main.c src/patch_bay_websocket.cpp ${VENDOR}/cjson/cJSON.c
  ${PROCESS_SOURCE})
target_compile_options(hydra-native-patch-bay PRIVATE ${WARNINGS})
set_target_properties(hydra-native-patch-bay PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(hydra-native-patch-bay PRIVATE ${VENDOR}/cjson)
target_link_libraries(hydra-native-patch-bay PRIVATE datachannel-static pthread)

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
target_link_libraries(hydra-audio-midi PUBLIC SDL3::SDL3 gles)

# ---------------------------------------------------------------- programs

add_executable(hydra-native src/main.c src/engine.c src/bindings.c src/sandbox.c src/snapshot.c src/code_socket.c
  src/text.c src/video.c src/fetch.c
  src/thumbnails.c src/recorder.c src/patch_bay.c src/editor.c src/ui.c src/settings.c src/overlay.c
  src/keymap.c src/programs.c src/signature.c src/window.c ${PROCESS_SOURCE})
target_compile_options(hydra-native PRIVATE ${WARNINGS})
target_compile_definitions(hydra-native PRIVATE HYDRA_NATIVE_VERSION="${PROJECT_VERSION}")
target_include_directories(hydra-native PRIVATE ${VENDOR}/stb)
target_link_libraries(hydra-native PRIVATE quickjs hydra-audio-midi SDL3::SDL3 gles
  m ${CMAKE_DL_LIBS} pthread)
if(WIN32)
  target_sources(hydra-native PRIVATE assets/icon/hydra-native.rc) # the icon of the file
  target_link_libraries(hydra-native PRIVATE ws2_32) # the code socket
endif()
if(TARGET libremidi) # C++, so the C++ driver links the program
  set_target_properties(hydra-native PROPERTIES LINKER_LANGUAGE CXX)
endif()

# Starts the shader compile of src/programs.c next to a hidden window and compiles two programs.
add_executable(programs-test test/programs_test.c src/programs.c src/window.c)
target_compile_options(programs-test PRIVATE ${WARNINGS})
target_link_libraries(programs-test PRIVATE SDL3::SDL3 gles pthread)

# ---------------------------------------------------------------- JavaScript

find_program(PNPM NAMES pnpm pnpm.cmd REQUIRED)
set(NODE_MODULES ${CMAKE_SOURCE_DIR}/node_modules)
add_custom_command(OUTPUT ${NODE_MODULES}/.modules.yaml
  COMMAND ${PNPM} install --frozen-lockfile
  DEPENDS ${CMAKE_SOURCE_DIR}/package.json ${CMAKE_SOURCE_DIR}/pnpm-lock.yaml
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
file(GLOB JS_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/js/*.js ${CMAKE_SOURCE_DIR}/js/hydra-editor/*)
# The metafile lists the bundled npm packages for the license files of the release archive.
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/hydra-bundle.js ${CMAKE_BINARY_DIR}/hydra-bundle.meta.json
  COMMAND ${NODE_MODULES}/.bin/esbuild$<$<BOOL:${WIN32}>:.cmd> js/main.js --bundle --format=iife --log-level=warning
    --inject:js/native.js
    --outfile=${CMAKE_BINARY_DIR}/hydra-bundle.js --metafile=${CMAKE_BINARY_DIR}/hydra-bundle.meta.json
    --alias:regl=./js/regl-shim.js --alias:raf-loop=./js/raf-loop-shim.js --alias:meyda=./js/meyda-shim.js
  DEPENDS ${JS_SOURCES} ${NODE_MODULES}/.modules.yaml
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_custom_target(javascript ALL DEPENDS ${CMAKE_BINARY_DIR}/hydra-bundle.js)

# ---------------------------------------------------------------- tests

enable_testing()
if(NOT WIN32) # the sanitizer runtimes of llvm-mingw are DLLs outside of the build folder
  set(SANITIZERS -fsanitize=address,undefined)
endif()

add_executable(editor-test EXCLUDE_FROM_ALL test/editor_test.c src/editor.c)
add_executable(ui-test EXCLUDE_FROM_ALL test/ui_test.c src/ui.c src/keymap.c src/editor.c src/settings.c
  src/signature.c)
add_executable(audio-test EXCLUDE_FROM_ALL test/audio_test.c src/audio_processing.c)
add_executable(process-test EXCLUDE_FROM_ALL test/process_test.c ${PROCESS_SOURCE})
target_link_libraries(ui-test PRIVATE quickjs SDL3::SDL3 m ${CMAKE_DL_LIBS} pthread)
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
