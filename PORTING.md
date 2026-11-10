# Porting hydra-native to macOS and Windows

hydra-native runs on Linux. This file lists what still ties it to Linux, module by module, and the library that
replaces each part on macOS and Windows. The libraries are the ones that we agreed on: ANGLE, miniaudio,
libremidi, libcurl and quickjs-ng. Video files, recording and the webRTC patch bay keep their child
processes (ffmpeg, Node.js) on all three systems; "Media on macOS and Windows" below gives the reasons.

## Status of the groundwork

- Screen capture has one interface, src/capture.h, with backends for Linux (src/capture_linux.c), macOS
  (src/capture_macos.m, ScreenCaptureKit) and Windows (src/capture_windows.cpp, Windows.Graphics.Capture).
- Child processes with pipes, process groups and job objects have one interface, src/process.h, with a POSIX
  backend (src/process_posix.c) and a Windows backend (src/process_windows.c).
- The audio input (src/audio_input.h) and MIDI (src/midi.h) have one interface each, with backends for Linux
  and for macOS and Windows. On macOS and Windows, CMake compiles them as the object library hydra-audio-midi,
  without a program yet, and builds the WebRTC audio processing as build/libhydra-webrtc-apm.
- Gallery sharing runs in JS (`__shareSketch` with fetch), so it needs no platform code.
- No Python runs at run time. Python is only for some tests.
- The build uses CMake (CMakeLists.txt); `make` is a thin wrapper. CMake downloads the vendored libraries with
  pinned SHA-256 hashes. SDL3 comes from the system on Linux, from Homebrew on macOS (`brew install sdl3`), and
  from the Visual C++ package of the SDL project on Windows (a pinned download).
- The window and the OpenGL ES 3.0 context come from src/window.c on all three systems: EGL of the system
  (Mesa) on Linux, EGL on ANGLE elsewhere. test/window_test.c opens a hidden window, compiles a GLSL ES 3.00
  shader, draws one pass into a framebuffer and reads the pixel back; CI runs it on macOS and Windows.
- The settings file is in ~/Library/Application Support/hydra-native on macOS and in %APPDATA%\hydra-native on
  Windows (SDL_GetPrefPath). The default font is Menlo on macOS and Consolas on Windows. The settings menu
  lists other fonts only through fontconfig, so on macOS and Windows it shows none; a "font" path in
  settings.json works. CoreText and DirectWrite lists are not done.
- On macOS and Windows, CMake builds the window test and compiles the ported sources (editor, overlay, menu,
  settings, signatures; on macOS only, because they still include POSIX headers) as a check. It does not link
  the program yet. tools/package.sh has the macOS (.app in a zip) and Windows (zip) branches, untested.

## ANGLE

macOS and Windows get OpenGL ES 3.0 from ANGLE: on Metal on macOS, on Direct3D 11 on Windows. The reasons for
ANGLE: the GLES code of the renderer, the overlay and p5 WEBGL stays as it is on all three systems, and ANGLE on
Metal is the GLES that Chrome and Safari use for WebGL, so hydra sketches run on the same layer as in the
browser.

ANGLE comes as a pinned prebuilt archive, not as an own build. The archives are the release angle-9684da7 of
https://github.com/jeremyfa/build-angle (ANGLE commit 9684da72c67be785ed049e4c5704964b5d6e1bcb, with libEGL,
libGLESv2, the headers and the import libraries), which the public GitHub Actions workflow of that repository
builds from the upstream commit; CMakeLists.txt checks their SHA-256 hashes. An own build needs depot_tools,
gclient and GN, and on the Windows runners a patch for the pinned Windows SDK; it takes about an hour for each
system and would use most of the CI budget of a round. To build ANGLE here instead, run the commands of
setup-angle-mac.sh and build-angle-windows.bat of that repository with the same commit, and put include/, lib/
and bin/ into vendor/angle.

The program loads ANGLE from its own folder: src/window.c gives SDL the paths of libEGL and libGLESv2 next to
the program, and the program links the same libGLESv2 (rpath @executable_path on macOS). On Windows,
d3dcompiler_47.dll comes with ANGLE.

## By module

| Part                           | Linux now                            | macOS and Windows                           |
| ------------------------------ | ------------------------------------ | ------------------------------------------- |
| window, input, clipboard, IME  | SDL3                                 | SDL3 (the same code on all three)           |
| GLES 3 context                 | EGL with Mesa                        | ANGLE (Metal, Direct3D 11)                  |
| curl processes                 | SDL_Process, memfd for the body      | SDL_Process, a pipe or file for the body    |
| program folder, downloads      | SDL_GetBasePath, SDL_GetUserFolder   | the same                                    |
| settings file                  | XDG_CONFIG_HOME/hydra-native         | SDL_GetPrefPath                             |
| downloads (fetch, loadScript)  | curl processes (src/fetch.c)         | libcurl in the process                      |
| JavaScript                     | QuickJS                              | quickjs-ng (also builds with MSVC)          |
| audio input and `a`            | PulseAudio (src/audio_input_pulse.c) | miniaudio (src/audio_input_miniaudio.c)     |
| WebRTC audio processing        | system libwebrtc-audio-processing-1  | version 1.3, built from source by CMake     |
| analog gain control            | PulseAudio source volume             | Core Audio, IAudioEndpointVolume            |
| MIDI                           | ALSA sequencer (src/midi.c)          | libremidi (src/midi_libremidi.cc)           |
| media child processes          | posix_spawn, process group           | posix_spawn; CreateProcess, job object      |
| camera                         | gst-launch-1.0 process               | SDL3 camera API                             |
| initVideo, initImage, recorder | ffmpeg processes                     | ffmpeg processes (ffmpeg in PATH)           |
| screen capture                 | portal and Mutter (src/capture.h)    | ScreenCaptureKit, Windows.Graphics.Capture  |
| webRTC patch bay               | node, @roamhq/wrtc, Unix socket      | the same; a named pipe on Windows           |
| fonts                          | fontconfig (fc-match, fc-list)       | CoreText, DirectWrite, or bundled fonts     |
| code socket (--send)           | Unix socket                          | Unix socket on macOS, named pipe on Windows |
| opening URLs                   | SDL_OpenURL                          | the same                                    |
| signals at exit                | SIGINT, SIGTERM                      | SDL_EVENT_QUIT, console control handler     |

## Audio input and MIDI

The audio input has one interface, src/audio_input.h: 10 ms frames of mono samples at 48 kHz, and the volume of
the capture device as a `struct source_volume` (src/analog_agc.h) for the analog gain control. Linux keeps the
simple API of PulseAudio (src/audio_input_pulse.c), because it takes the source names of PulseAudio and PipeWire,
also @DEFAULT_MONITOR@ for the system output, and it reads 10 ms fragments without a ring buffer between.
macOS and Windows use miniaudio 0.11.25 (src/audio_input_miniaudio.c). miniaudio is one file in the public
domain, and it has the WASAPI loopback, which the audio capture of SDL3 does not have. It also gives the device
ID that the volume control needs. `--audio TEXT` selects the first input whose name contains TEXT; without a
match, hydra-native lists the inputs.

The system output (`--audio @DEFAULT_MONITOR@`):

- Linux: the monitor source of the default sink in PulseAudio or PipeWire.
- Windows: the WASAPI loopback of the default output.
- macOS has no loopback capture in Core Audio. Install a virtual loopback device such as BlackHole, make it the
  output (or part of a multi-output device in Audio MIDI Setup), and give its name: `--audio BlackHole`. Two
  other ways are not done: the audio of ScreenCaptureKit (macOS 13 and newer) and the process taps of Core Audio
  (AudioHardwareCreateProcessTap, macOS 14.2 and newer). Both need the permission for screen and audio recording.

The analog gain control sets the volume of the input device on every system, and restores the saved volume at
the end; `--no-analog-agc` turns it off. macOS: kAudioDevicePropertyVolumeScalar in the input scope, of the main
element when it is settable, else of each channel (as Chrome does). Windows: the master volume level scalar of
IAudioEndpointVolume. The system output keeps its volume. src/audio_volume.h selects the code of the system.

WebRTC audio processing: macOS and Windows build the sources of webrtc-audio-processing 1.3 with
cmake/webrtc-audio-processing.cmake, which follows the meson.build files of the release (meson is not needed).
Abseil is used only through its headers. On Linux, `cmake -DHYDRA_WEBRTC_APM_FROM_SOURCE=ON` builds the same
sources, and audio-test gives the same numbers as with the system library. src/audio_processing.c stays the
fallback when the library is missing.

MIDI has one interface, src/midi.h. Linux keeps the ALSA sequencer (src/midi.c), with the arrival time of each
event in the kernel. macOS and Windows use libremidi 5.4.3 (src/midi_libremidi.cc) with CoreMIDI and WinMM,
because libremidi reports port changes through its observer and gives the time stamps of the system (the host
time of a CoreMIDI packet, the time of the WinMM driver); RtMidi has neither. A message gets the time stamp of the
system when it is plausible, else the time of the callback. midi_poll() converts it to the clock of
performance.now(), which the caller passes. hydra-native opens every input, also inputs that appear later, and
opens an output on its first message.

On Linux, `cmake -DHYDRA_MINIAUDIO_LIBREMIDI=ON` builds the miniaudio and libremidi code (with PulseAudio and the
ALSA sequencer) to test it: midi-test passes with it. The code for Core Audio, WASAPI, IAudioEndpointVolume,
CoreMIDI and WinMM is not tried on a Mac or a Windows PC yet.

## Notes per platform

Child processes: the curl downloads run through SDL_Process. Video sources, the recorder, the patch bay and the
screen capture use src/process.h, because SDL_Process cannot start a process group: `initCommand` runs shell
pipelines such as `yt-dlp | ffmpeg`, and video_close stops the whole group. On POSIX, posix_spawn starts the
child in a new process group, and process_kill sends SIGTERM to the group. On Windows, CreateProcessW starts the
child suspended in a job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, and process_kill calls
TerminateJobObject. The children inherit only their three standard handles (PROC_THREAD_ATTRIBUTE_HANDLE_LIST)
and open no console window (CREATE_NO_WINDOW). Pipes are C runtime descriptors on Windows too, so the callers
share one stream API. test/process_test.c checks the interface with copies of itself. The settings stay
in XDG_CONFIG_HOME on Linux, because SDL_GetPrefPath gives the data folder (XDG_DATA_HOME) there.

macOS: ANGLE on Metal gives GLES 3. ScreenCaptureKit needs the screen recording permission, and the camera
needs the camera permission in Info.plist. The microphone needs NSMicrophoneUsageDescription in Info.plist; a
program started from a terminal uses the microphone permission of the terminal.

Blend mode difference (the autocomplete list, the signature help, and the code with "codeBackgroundBlend" or
"codeTextBlend" set to "difference", src/overlay.c): with GL_KHR_blend_equation_advanced, GL_DIFFERENCE_KHR
blends in one pass; without it, a shader reads a copy of the window below each difference layer
(glCopyTexSubImage2D of its bounding rectangle). Mesa on this machine has the extension. ANGLE on Direct3D 11 does
not offer it, so Windows takes the shader path. Whether ANGLE on Metal offers it is not checked; the shader path
works there in any case, because it needs only GLES 3.0. At 1920x1080 with both code layers on difference and
the code of the artrio sketch, both paths cost about 0.8 ms per frame more than normal blending (best of four
runs: 239 fps normal, 200 fps extension, 202 fps shader), and the overlay CPU time grows from 0.2 to 0.35 ms.

Windows: ANGLE on Direct3D 11 gives GLES 3. Windows.Graphics.Capture needs Windows 10 1903 or newer. quickjs-ng
replaces QuickJS, which does not build with MSVC. The code socket becomes a named pipe. The microphone needs
"Let desktop apps access your microphone" in the privacy settings. WinMM opens a MIDI device for one program at a
time, so a device that another program uses is in the list but sends nothing.

The camera: macOS and Windows use the SDL3 camera API (`video_open_camera` in src/video.c). Linux stays with
GStreamer, because SDL 3.4.2 was slower in a measurement there. The integrated camera of this machine sends MJPEG
at 1280x720 (11 to 12 fps in low light). In `initCam()`, the GStreamer thread that reads and decodes the frames
used 1.5 s of CPU in 10 s; the SDL camera thread, which decodes and converts to RGBA, used 2.1 s for the same
frames (about 13 ms against 18 ms for each frame). The SDL camera is the better choice on macOS and Windows,
where no GStreamer is installed.

## Media on macOS and Windows

The camera uses SDL3 instead of ffmpeg with its avfoundation and dshow devices. SDL is in the process already,
needs no program in PATH, lists the cameras by name in the order of `initCam(index)`, and asks for the camera
permission itself. SDL converts and scales the frames to RGBA of the requested size in its camera thread, so the
render thread only copies the newest frame, and no pipe carries 3.7 MB per frame. ffmpeg would need a frame rate
and a size that the device supports exactly (avfoundation fails otherwise), and dshow selects a device by its
name, which needs a parse of `ffmpeg -list_devices`. The SDL camera path ran on Linux with the integrated camera.

Shell commands: `initCommand` runs /bin/sh -c on Linux and macOS and cmd.exe /d /s /c on Windows. A command for
Windows uses the syntax of cmd.exe: double quotes, no single quotes, `%NAME%` expands a variable, and the pipe
`|` works. js/sources.js quotes with double quotes on Windows for its own commands.

ffmpeg: initVideo, initImage and vidRecorder need ffmpeg in PATH on all systems (from Homebrew on macOS, for
example from `winget install Gyan.FFmpeg` on Windows). YouTube links also need yt-dlp. When a program is
missing, the error bar names it. macOS starts programs from the Finder with a PATH of only the system folders, so
src/process_posix.c adds /opt/homebrew/bin and /usr/local/bin.

Screen capture permissions: on macOS, `initMonitor`, `initArea` and `initWindow` need the Screen Recording
permission (System Settings, Privacy & Security). The first call shows the prompt of the system, and the capture
works after the user allowed it and started it again. macOS gives the permission to the responsible app, which is
hydra-native or the terminal that started it, because the capture helper is its child. `initScreen` uses the
system picker SCContentSharingPicker (macOS 14 or newer), in which the user chooses each time. A packaged app
needs NSCameraUsageDescription in its Info.plist for the camera; without it, macOS ends the program at the first
camera access. On Windows, Windows.Graphics.Capture needs no permission. Windows draws a yellow border around the
captured window or monitor, and `initScreen` opens the GraphicsCapturePicker. The Windows helper works in
physical pixels, so `initArea` takes physical pixel coordinates of the virtual screen.

Monitor numbers: `initMonitor(index)` counts in the order of the system: org.gnome.Mutter.DisplayConfig on Linux,
SCShareableContent on macOS and EnumDisplayMonitors on Windows. Areas: Mutter and ScreenCaptureKit scale the
area. Windows crops the monitor that contains the top left corner of the area and scales with nearest-neighbor
sampling. `initWindow` records the frontmost window after the delay: the first normal window of
CGWindowListCopyWindowInfo on macOS, GetForegroundWindow on Windows.

The patch bay: Node.js stays a run-time requirement on all systems (`node` in PATH). tools/patch-bay.mjs listens
on a local socket with net.Server: a Unix socket in XDG_RUNTIME_DIR, TMPDIR or /tmp on Linux and macOS, the named
pipe `\\.\pipe\hydra-native-patch-bay-PID` on Windows. JSON lines still go through the standard input and
output of the helper. @roamhq/wrtc 0.8.0 has prebuilt packages for darwin-arm64, darwin-x64 and win32-x64
(version 0.8.0) and for linux-x64 and linux-arm64 (version 0.8.1). pnpm installs only the package of the system
that runs it, because of their os and cpu fields. Thus a package for macOS or Windows must come from
`pnpm install` on that system, or from `supportedArchitectures` in pnpm-workspace.yaml. tools/package.sh copies
the package of the system that runs it (wrtc-linux-x64, wrtc-darwin-arm64 or wrtc-win32-x64) into
node_modules/@roamhq next to the bundled tools/patch-bay.mjs. On macOS, a
signed and notarized app must sign wrtc.node with the identity of the app, because it is code in the bundle.

The icon: assets/icon/favicon.ico has only 16, 32 and 48 px images. The Linux package uses them as hicolor icons
and as the window icon. assets/icon/hydra-native.icns (macOS) holds the 16 and 32 px images, also as 16 px @2x;
assets/icon/hydra-native.rc puts favicon.ico into the Windows program. The Dock and the Windows taskbar scale
these small images up, so they look soft. A larger source image (an SVG or a PNG of 1024 px) would give sharp
icons up to 1024 px (.icns) and 256 px (.ico).
