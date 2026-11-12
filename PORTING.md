# Porting hydra-native to macOS and Windows

hydra-native is released for Linux. This file gives the state of the macOS and Windows ports and the facts that
a porter needs. docs/manual.md describes what a user sees on each system.

## Status

- Linux (x86_64): complete.
- macOS (arm64): a preview. CMake builds the program, the capture tool, the JS bundle and the unit tests except
  the MIDI test (`brew install cmake ninja pnpm sdl3`, then `make`), with the patch bay. CI renders
  test/check.js on ANGLE in a hidden window and packs hydra-native.app with an ad-hoc signature
  (tools/package.sh). Not tested on a Mac: the
  microphone, camera and screen recording permissions, the audio of the system output, CoreMIDI, the Dock icon,
  the code socket and the patch bay.
- Windows (x64): a preview. CMake builds the program, the capture tool, the patch bay helper, the JS bundle and
  the unit tests with llvm-mingw (README.md). CI runs the window test, the program check and the unit tests,
  and packs a zip with tools/package.sh. Not tested on a Windows desktop: the window with a GPU, the camera,
  the screen capture, the microphone, WinMM, the code socket, the file dialogs and the patch bay.

## Open work for Windows

- The system output as audio input is not available.
- The patch bay helper finds no CA certificates on Windows, so it does not check the certificate of a wss://
  signalling server. mbedTLS does not read the certificate store of Windows.
- hydra-native.exe is a console program, so a start from the Explorer also opens a console window.

## What differs from Linux

| Part            | Linux                                | macOS                        | Windows                   |
| --------------- | ------------------------------------ | ---------------------------- | ------------------------- |
| GLES 3 context  | EGL with Mesa                        | ANGLE on Metal               | ANGLE on Direct3D 11      |
| SDL3            | system package                       | Homebrew                     | pinned MinGW package      |
| settings folder | XDG_CONFIG_HOME/hydra-native         | SDL_GetPrefPath              | SDL_GetPrefPath           |
| default font    | DejaVu Sans Mono                     | Menlo                        | Consolas                  |
| font list       | fontconfig                           | none ("font" path works)     | none ("font" path works)  |
| audio input     | PulseAudio (src/audio_input_pulse.c) | SDL3 (src/audio_input_sdl.c) | SDL3                      |
| system output   | monitor source of the default sink   | ScreenCaptureKit, macOS 13   | not available             |
| MIDI            | ALSA sequencer (src/midi.c)          | libremidi with CoreMIDI      | libremidi with WinMM      |
| camera          | gst-launch-1.0 process               | SDL3 camera                  | SDL3 camera               |
| screen capture  | portal and Mutter                    | ScreenCaptureKit             | Windows.Graphics.Capture  |
| patch bay       | libdatachannel and ffmpeg            | libdatachannel and ffmpeg    | libdatachannel and ffmpeg |
| child processes | posix_spawn, process group           | posix_spawn, process group   | CreateProcess, job object |
| shell           | /bin/sh -c                           | /bin/sh -c                   | cmd.exe /d /s /c          |

ANGLE comes as a prebuilt archive of https://github.com/jeremyfa/build-angle, and CMakeLists.txt pins its
SHA-256 hash. The program loads libEGL and libGLESv2 from its own folder (src/window.c).

Blend mode difference (src/overlay.c) uses GL_KHR_blend_equation_advanced where the driver has it. ANGLE on
Direct3D 11 does not, so a shader reads a copy of the window below each difference layer there.

## macOS

- macOS starts programs from the Finder with a PATH of only the system folders, so src/process_posix.c adds
  /opt/homebrew/bin and /usr/local/bin for ffmpeg, yt-dlp and node.
- The capture helper is a child of hydra-native, so macOS gives the Screen Recording permission to hydra-native,
  or to the terminal that started it.
- The audio of the system output comes from `hydra-native-capture audio` (src/capture_macos.m), an SCStream of
  the main display with audio. It needs the Screen Recording permission and no microphone permission.
- The patch bay needs ffmpeg from Homebrew for the video, as initVideo does.

## Windows

- The toolchain is llvm-mingw: clang, libc++ and the UCRT C runtime of MinGW-w64. Upstream QuickJS does not
  build with Visual C++, and MinGW-w64 has most of the POSIX headers of the program. The programs link the C++
  runtime and winpthreads statically. C++/WinRT for the capture tool comes from cppwinrt.exe, which writes its
  headers from the metadata of the Windows that runs the build.
- The code socket is a Unix socket of Winsock (Windows 10 1803 or newer), at `%TEMP%\hydra-native.sock` by
  default. A named pipe has no half-close, which ends the code of `--send`.
- The local socket of the patch bay helper is a named pipe. Windows cannot poll pipes, so the helper checks them
  with PeekNamedPipe and waits 1 ms between two checks (src/patch_bay_main.c).
- The C runtime raises SIGINT for Ctrl+C in the console. Windows sends no SIGTERM.
- A command of `initCommand` uses the syntax of cmd.exe: double quotes, no single quotes, and `%NAME%` expands a
  variable.
- Windows.Graphics.Capture needs Windows 10 1903 or newer and no permission. Windows draws a yellow border
  around the captured window or monitor.
- The capture helper works in physical pixels, so `initArea` takes physical pixel coordinates of the virtual
  screen. It crops the monitor that contains the top left corner of the area.
- `initMonitor(index)` counts in the order of EnumDisplayMonitors, and `initWindow` records the window of
  GetForegroundWindow.
- The microphone needs "Let desktop apps access your microphone" in the privacy settings.
- WinMM opens a MIDI device for one program at a time, so a device that another program uses sends nothing.

## Icons

assets/icon/favicon.ico and assets/icon/hydra-native.icns hold only images of 16, 32 and 48 px, so the Dock and
the Windows taskbar scale them up. A source image of 1024 px would give sharp icons.
