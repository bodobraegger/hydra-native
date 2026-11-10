#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <GLES3/gl3.h>

#define BARK_BAND_COUNT 24

// Video sources: a child process writes raw RGBA frames of a fixed size to its stdout (video_open runs a shell
// command, see src/process.h), or a stream of the patch bay brings them (video_open_stream owns `stream`).
int video_open(const char *command, int width, int height);
int video_open_stream(int stream, int width, int height);
// Camera `index` of the names that video_camera_names lists, in RGBA frames of width x height: GStreamer with the
// /dev/video* capture nodes on Linux, the SDL camera elsewhere (AVFoundation on macOS, Media Foundation on Windows).
int video_open_camera(int index, int width, int height);
int video_camera_names(const char **names, int capacity);
bool video_upload(int handle, GLuint texture);
bool video_is_running(int handle);
bool video_size(int handle, int *width, int *height);
bool video_copy_frame(int handle, unsigned char *pixels);
void video_close(int handle);
void video_close_all(void);

// Audio input: Meyda-compatible "loudness" features of the audio input (src/audio_input.h).
bool audio_start(const char *device, const char *program_directory); // the folder of hydra-native-capture
// The input of `a` from now on: a device name as in audio_devices(), "" for the default, AUDIO_NONE for none.
// The running input closes and the new one opens; before audio_start() it only sets the name.
void audio_set_device(const char *device);
void audio_set_gain(int percent); // multiplies the samples before the analysis
struct audio_device;
int audio_devices(struct audio_device *devices, int capacity); // see audio_input_devices() of src/audio_input.h
bool audio_features(float specific[BARK_BAND_COUNT], float *total);

// The options of curl for each request: only HTTP and HTTPS, also after a redirect, so that no URL reads a local file.
#define CURL_PROTOCOL_OPTIONS "--proto", "=http,https", "--proto-redir", "=http,https"

// Downloads with curl, and other child processes; fetch_poll calls `done` once per finished process with its
// output and whether it exited with status 0.
int fetch_start(const char *url);
// A request of fetch() and XMLHttpRequest; the output is the response header blocks and then the body.
int http_start(const char *url, const char *method, const char *const *headers, size_t header_count, const void *body,
               size_t body_length);
void fetch_stop_all(void); // at exit, without output of the stopped processes
void fetch_poll(void (*done)(int id, bool ok, const char *data, size_t length));

// Recording of the hydra output to a WebM file through ffmpeg (vidRecorder).
#define RECORD_FPS 25 // captureStream(25) in hydra-synth
bool recorder_start(const char *path, int width, int height);
bool recorder_active(void);
void recorder_frame(double now); // call after the hydra frame and before the code overlay
bool recorder_stop(void); // waits for ffmpeg; false when it failed
// The output frames for the patch bay (25 fps, rows from the bottom) into `stream`, which the publisher closes.
bool publisher_start(int stream, int width, int height);
bool publisher_active(void);
void publisher_stop(void);
// The webRTC patch bay (tools/patch-bay.mjs): one helper process with JSON lines in and out.
bool patch_bay_start(const char *script, const char *server, const char *room);
bool patch_bay_send(const char *line);
void patch_bay_poll(void (*line)(const char *text));
bool patch_bay_publish(bool on, int width, int height); // opens or closes the output channel of the helper
int patch_bay_connect(const char *channel); // a stream to the local socket of the helper, or -1
void patch_bay_stop(void);

// Shader programs that compile without stopping the frames (src/programs.c). program_create() returns at once;
// program_status() is PROGRAM_COMPILING until the program is ready, unless `wait` is true. PROGRAM_FAILED deletes
// the program and sets `*error` to the malloc'd log.
enum program_status { PROGRAM_FAILED, PROGRAM_COMPILING, PROGRAM_LINKED };
void programs_start(void);          // after the GL context exists
void programs_stop(void);           // before the window closes
const char *programs_method(void);  // "thread", "parallel" or "blocking"
GLuint program_create(const char *vertex_source, const char *fragment_source);
enum program_status program_status(GLuint program, bool wait, char **error);
