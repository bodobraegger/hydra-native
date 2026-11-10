# Porting hydra-native to macOS and Windows

hydra-native runs on Linux. This file lists what still ties it to Linux, module by module, and the library that
replaces each part on macOS and Windows. The libraries are the ones that we agreed on: ANGLE, miniaudio or SDL3
audio, libremidi, libav in the process, libcurl, quickjs-ng and libdatachannel.

## Status of the groundwork

- Screen capture has one interface, src/capture.h, with a Linux backend (src/capture_linux.c). A macOS
  backend implements `capture_run` with ScreenCaptureKit, a Windows backend with Windows.Graphics.Capture.
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
| camera, video files, recording | gst-launch-1.0 and ffmpeg processes | libav in the process, platform cameras      |
| screen capture                 | portal and Mutter (src/capture.h)   | ScreenCaptureKit, Windows.Graphics.Capture  |
| webRTC patch bay               | node with @roamhq/wrtc              | libdatachannel in the process               |
| fonts                          | fontconfig (fc-match, fc-list)      | CoreText, DirectWrite, or bundled fonts     |
| code socket (--send)           | Unix socket                         | Unix socket on macOS, named pipe on Windows |
| opening URLs                   | SDL_OpenURL                         | the same                                    |
| signals at exit                | SIGINT, SIGTERM                     | SDL_EVENT_QUIT, console control handler     |

## Notes per platform

Child processes: the curl downloads run through SDL_Process. Video sources, the recorder, the patch bay and the
screen capture still use fork, because SDL_Process cannot start a process group (`initCommand` runs shell
pipelines such as `yt-dlp | ffmpeg`, and video_close stops the whole group), and because these modules are
Linux only: the table above replaces them with libav, libdatachannel and the capture backends. The settings stay
in XDG_CONFIG_HOME on Linux, because SDL_GetPrefPath gives the data folder (XDG_DATA_HOME) there.

macOS: ANGLE on Metal gives GLES 3. ScreenCaptureKit needs the screen recording permission, and the camera
needs the camera permission in Info.plist. The analog gain control sets the input volume through Core Audio.

Windows: ANGLE on Direct3D 11 gives GLES 3. Windows.Graphics.Capture needs Windows 10 1903 or newer. quickjs-ng
replaces QuickJS, which does not build with MSVC. The code socket becomes a named pipe.

The camera: the SDL3 camera API is a candidate for the camera on all three platforms, but on Linux it stays
with GStreamer, because SDL 3.4.2 was slower in a measurement. The integrated camera of this machine sends MJPEG
at 1280x720 (11 to 12 fps in low light). In `initCam()`, the GStreamer thread that reads and decodes the frames
used 1.5 s of CPU in 10 s; the SDL camera thread, which decodes and converts to RGBA, used 2.1 s for the same
frames (about 13 ms against 18 ms for each frame). The SDL camera is the better choice on macOS and Windows,
where no GStreamer is installed.

The icon: assets/icon/favicon.ico has only 16, 32 and 48 px images. The Linux package uses them as hicolor icons
and as the window icon. A macOS .icns needs images up to 1024 px, and a Windows .ico for the taskbar and the
installer needs 256 px. Both need a larger source image (an SVG or a PNG of 1024 px) before the ports.
