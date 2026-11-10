# Porting hydra-native to macOS and Windows

hydra-native runs on Linux. This file lists what still ties it to Linux, module by module, and the library that
replaces each part on macOS and Windows. The libraries are the ones that we agreed on: ANGLE, miniaudio or SDL3
audio, libremidi, libav in the process, libcurl, quickjs-ng and libdatachannel.

## Status of the groundwork

- Screen capture has one interface, src/capture.h, with a Linux backend (src/capture_linux.c). A macOS
  backend implements `capture_run` with ScreenCaptureKit, a Windows backend with Windows.Graphics.Capture.
- Gallery sharing runs in JS (`__shareSketch` with fetch), so it needs no platform code.
- No Python runs at run time. Python is only for some tests.
- Not done yet: SDL3 and a CMake build. The system has no SDL3 development files, and SDL3 builds only with
  CMake. CMake is not installed, and the permission system refused to run a CMake that a package manager
  downloads (`uvx cmake`). With CMake available, the plan is: SDL3 3.2 from pinned source in vendor/, a
  CMakeLists.txt that fetches the vendored libraries as the Makefile does now, and `make` as a thin wrapper.

## By module

| Part                            | Linux now                           | macOS and Windows                           |
| ------------------------------- | ----------------------------------- | ------------------------------------------- |
| window, input, clipboard, IME   | SDL3                                | SDL3 (the same code on all three)           |
| GLES 3 context                  | EGL with Mesa                       | ANGLE (Metal, Direct3D 11)                  |
| child processes and pipes       | fork, exec, pipe2, memfd            | SDL3 SDL_Process                            |
| paths of settings and resources | XDG_CONFIG_HOME, /proc/self/exe     | SDL_GetPrefPath, SDL_GetBasePath            |
| downloads (fetch, loadScript)   | curl processes (src/fetch.c)        | libcurl in the process                      |
| JavaScript                      | QuickJS                             | quickjs-ng (also builds with MSVC)          |
| audio input and `a`             | PulseAudio simple API (src/audio.c) | miniaudio or SDL3 audio                     |
| WebRTC audio processing         | system libwebrtc-audio-processing-1 | the same library, built and bundled         |
| analog gain control             | PulseAudio source volume            | Core Audio, WASAPI endpoint volume          |
| MIDI                            | ALSA sequencer (src/midi.c)         | libremidi                                   |
| camera, video files, recording  | gst-launch-1.0 and ffmpeg processes | libav in the process, platform cameras      |
| screen capture                  | portal and Mutter (src/capture.h)   | ScreenCaptureKit, Windows.Graphics.Capture  |
| webRTC patch bay                | node with @roamhq/wrtc              | libdatachannel in the process               |
| fonts                           | fontconfig (fc-match, fc-list)      | CoreText, DirectWrite, or bundled fonts     |
| code socket (--send)            | Unix socket                         | Unix socket on macOS, named pipe on Windows |
| opening URLs                    | xdg-open                            | SDL_OpenURL of SDL3                         |
| signals at exit                 | SIGINT, SIGTERM                     | SDL_EVENT_QUIT, console control handler     |

## Notes per platform

macOS: ANGLE on Metal gives GLES 3. ScreenCaptureKit needs the screen recording permission, and the camera
needs the camera permission in Info.plist. The analog gain control sets the input volume through Core Audio.

Windows: ANGLE on Direct3D 11 gives GLES 3. Windows.Graphics.Capture needs Windows 10 1903 or newer. quickjs-ng
replaces QuickJS, which does not build with MSVC. The code socket becomes a named pipe.

The camera: the SDL3 camera API is a candidate for the camera on all three platforms, but on Linux it stays
with GStreamer until a measurement shows the same speed with the same formats (MJPEG at 1280x720 and 30 fps).

The icon: assets/icon/favicon.ico has only 16, 32 and 48 px images. The Linux package uses them as hicolor icons
and as the window icon. A macOS .icns needs images up to 1024 px, and a Windows .ico for the taskbar and the
installer needs 256 px. Both need a larger source image (an SVG or a PNG of 1024 px) before the ports.
