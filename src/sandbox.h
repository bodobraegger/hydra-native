// The sandbox of shared code, and the shell commands of the media sources, which hold no code of a sketch.
//
// Shared code (a sketch URL, a sketch of the gallery, an extension, a remote script) turns the sandbox on with
// native.sandbox(). It stays on until the program ends, because shared code can leave timers and changed globals
// behind. In the sandbox, a sketch cannot run shell commands or open local media files, and readFile and writeFile
// work only in the cache directory, so that shared code gets the access of a web page.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define SANDBOX_REASON "shared code ran in this session (restart hydra-native to use it)"

void sandbox_enter(void);
bool sandbox_active(void);
bool sandbox_allows_path(const char *path); // for readFile and writeFile

enum command_status {
    COMMAND_OK,
    COMMAND_INVALID,   // an argument that no command can hold
    COMMAND_SANDBOXED, // a local file, which the sandbox refuses
    COMMAND_FAILED,
};

// The protocols of ffmpeg and ffprobe in the sandbox, so that no URL of shared code opens a local file, also not
// from a playlist.
#define SANDBOX_PROTOCOLS "http,https,tcp,tls,crypto"

// The position in seconds, the speed (a factor of the realtime filter) and the loop of a "video" of
// sandbox_media_command.
struct media_playback {
    double start;
    double rate;
    bool loop;
};

// The input of ffmpeg and ffprobe for `url`: the path of a file:// URL, or the URL itself; NULL for a local file in
// the sandbox.
const char *sandbox_media_input(const char *url);
// The command of initVideo and initImage: ffmpeg plays `url` (a file or a stream; "video", as in `playback`), takes
// the first frame ("image"), or plays the output of yt-dlp ("youtube"), as RGBA frames of width x height.
enum command_status sandbox_media_command(char *command, size_t size, const char *url, const char *kind, int width,
                                          int height, const struct media_playback *playback);
// The command of the screen capture: hydra-native-capture of `program_directory` (src/capture_main.c) with the
// mode "picker", "monitor", "area" or "window" and the numbers of its usage.
enum command_status sandbox_capture_command(char *command, size_t size, const char *program_directory,
                                            const char *mode, const int *numbers, int count);
