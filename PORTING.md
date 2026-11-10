# Porting hydra-native to macOS and Windows

hydra-native runs on Linux. This file lists what still ties it to Linux, module by module, and the library that
replaces each part on macOS and Windows. The libraries are the ones that we agreed on: ANGLE, miniaudio or SDL3
audio, libremidi, libcurl and quickjs-ng. Video files, recording and the webRTC patch bay keep their child
processes (ffmpeg, Node.js) on all three systems; "Media on macOS and Windows" below gives the reasons.

## Status of the groundwork

- Screen capture has one interface, src/capture.h, with backends for Linux (src/capture_linux.c), macOS
  (src/capture_macos.m, ScreenCaptureKit) and Windows (src/capture_windows.cpp, Windows.Graphics.Capture).
- Child processes with pipes, process groups and job objects have one interface, src/process.h, with a POSIX
  backend (src/process_posix.c) and a Windows backend (src/process_windows.c).
- Gallery sharing runs in JS (`__shareSketch` with fetch), so it needs no platform code.
- No Python runs at run time. Python is only for some tests.
- The build uses CMake (CMakeLists.txt) with SDL3 from the system; `make` is a thin wrapper. CMake downloads
  the vendored libraries with pinned SHA-256 hashes. On macOS and Windows, SDL3 can come from a pinned
  source download in the same way, because those systems have no SDL3 package.

## By module

| Part                           | Linux now                           | macOS and Windows                           |
| ------------------------------ | ----------------------------------- | ------------------------------------------- |
| window, input, clipboard, IME  | SDL3                                | SDL3 (the same code on all three)           |
| GLES 3 context                 | EGL with Mesa                       | ANGLE (Metal, Direct3D 11)                  |
| curl processes                 | SDL_Process, memfd for the body     | SDL_Process, a pipe or file for the body    |
| program folder, downloads      | SDL_GetBasePath, SDL_GetUserFolder  | the same                                    |
| settings file                  | XDG_CONFIG_HOME/hydra-native        | SDL_GetPrefPath                             |
| downloads (fetch, loadScript)  | curl processes (src/fetch.c)        | libcurl in the process                      |
| JavaScript                     | QuickJS                             | quickjs-ng (also builds with MSVC)          |
| audio input and `a`            | PulseAudio simple API (src/audio.c) | miniaudio or SDL3 audio                     |
| WebRTC audio processing        | system libwebrtc-audio-processing-1 | the same library, built and bundled         |
| analog gain control            | PulseAudio source volume            | Core Audio, WASAPI endpoint volume          |
| MIDI                           | ALSA sequencer (src/midi.c)         | libremidi                                   |
| media child processes          | posix_spawn, process group          | posix_spawn; CreateProcess, job object      |
| camera                         | gst-launch-1.0 process              | SDL3 camera API                             |
| initVideo, initImage, recorder | ffmpeg processes                    | ffmpeg processes (ffmpeg in PATH)           |
| screen capture                 | portal and Mutter (src/capture.h)   | ScreenCaptureKit, Windows.Graphics.Capture  |
| webRTC patch bay               | node, @roamhq/wrtc, Unix socket     | the same; a named pipe on Windows           |
| fonts                          | fontconfig (fc-match, fc-list)      | CoreText, DirectWrite, or bundled fonts     |
| code socket (--send)           | Unix socket                         | Unix socket on macOS, named pipe on Windows |
| opening URLs                   | SDL_OpenURL                         | the same                                    |
| signals at exit                | SIGINT, SIGTERM                     | SDL_EVENT_QUIT, console control handler     |

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
needs the camera permission in Info.plist. The analog gain control sets the input volume through Core Audio.

Blend mode difference (the autocomplete list, the signature help, and the code with "codeBackgroundBlend" or
"codeTextBlend" set to "difference", src/overlay.c): with GL_KHR_blend_equation_advanced, GL_DIFFERENCE_KHR
blends in one pass; without it, a shader reads a copy of the window below each difference layer
(glCopyTexSubImage2D of its bounding rectangle). Mesa on this machine has the extension. ANGLE on Direct3D 11 does
not offer it, so Windows takes the shader path. Whether ANGLE on Metal offers it is not checked; the shader path
works there in any case, because it needs only GLES 3.0. At 1920x1080 with both code layers on difference and
the code of the artrio sketch, both paths cost about 0.8 ms per frame more than normal blending (best of four
runs: 239 fps normal, 200 fps extension, 202 fps shader), and the overlay CPU time grows from 0.2 to 0.35 ms.

Windows: ANGLE on Direct3D 11 gives GLES 3. Windows.Graphics.Capture needs Windows 10 1903 or newer. quickjs-ng
replaces QuickJS, which does not build with MSVC. The code socket becomes a named pipe.

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
node_modules/@roamhq/wrtc-linux-x64 by name; a package for another system copies wrtc-darwin-arm64 or
wrtc-win32-x64 into node_modules/@roamhq next to the bundled tools/patch-bay.mjs in the same way. On macOS, a
signed and notarized app must sign wrtc.node with the identity of the app, because it is code in the bundle.

The icon: assets/icon/favicon.ico has only 16, 32 and 48 px images. The Linux package uses them as hicolor icons
and as the window icon. A macOS .icns needs images up to 1024 px, and a Windows .ico for the taskbar and the
installer needs 256 px. Both need a larger source image (an SVG or a PNG of 1024 px) before the ports.
