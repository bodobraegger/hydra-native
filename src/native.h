#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <GLES3/gl3.h>

#define BARK_BAND_COUNT 24

// Video sources: a child process writes raw RGBA frames of a fixed size to its stdout (video_open runs a shell
// command, see src/process.h), or a stream of the patch bay brings them (video_open_stream owns `stream`).
#define MAX_VIDEO_SOURCES 16 // the handles are 0 to MAX_VIDEO_SOURCES - 1
int video_open(const char *command, int width, int height);
int video_open_stream(int stream, int width, int height);
// A command of sandbox_media_command. With `on_demand`, video_upload reads the frames instead of a thread.
int video_open_media(const char *command, int width, int height, bool on_demand);
// Camera `index` of the names that video_camera_names lists, in RGBA frames of width x height: GStreamer with the
// /dev/video* capture nodes on Linux, the SDL camera elsewhere (AVFoundation on macOS, Media Foundation on Windows).
int video_open_camera(int index, int width, int height);
int video_camera_names(const char **names, int capacity);
bool video_upload(int handle, GLuint texture, int frames); // frames: see video_open_media
bool video_is_running(int handle);
int video_frames(int handle); // the count of frames that the stream delivered
bool video_size(int handle, int *width, int *height);
bool video_copy_frame(int handle, unsigned char *pixels);
void video_close(int handle);
void video_close_all(void);

// Audio input: Meyda-compatible "loudness" features of the audio input (src/audio_input.h).
// The input of `a` from now on: a device name as in audio_devices(), "" for the default, AUDIO_NONE for none.
// The running input closes and the new one opens; before audio_start() it only sets the name.
void audio_set_device(const char *device);
bool audio_start(const char *program_directory); // the folder of hydra-native-capture; opens the device of above
void audio_set_gain(int percent); // multiplies the samples before the analysis
struct audio_device;
int audio_devices(struct audio_device *devices, int capacity); // see audio_input_devices() of src/audio_input.h
bool audio_features(float specific[BARK_BAND_COUNT], float *total);
// During a render to file (src/render.c) the features come from the samples of an audio file instead of the input:
// audio_feed_start() starts a new analysis with all features at 0, audio_feed() adds mono samples at
// AUDIO_SAMPLE_RATE (src/audio_processing.h), and audio_feed_stop() returns to the input.
void audio_feed_start(void);
void audio_feed(const float *samples, size_t count);
void audio_feed_stop(void);

// The options of curl for each request: only HTTP and HTTPS, also after a redirect, so that no URL reads a local file.
#define CURL_PROTOCOL_OPTIONS "--proto", "=http,https", "--proto-redir", "=http,https"

// Downloads with curl, and other child processes; fetch_poll calls `done` once per finished process with its
// output and whether it exited with status 0.
int fetch_start(const char *url);
// Another program, argv[0] in PATH, whose standard output fetch_poll collects.
int fetch_start_program(const char *const argv[]);
// A request of fetch() and XMLHttpRequest; the output is the response header blocks and then the body.
int http_start(const char *url, const char *method, const char *const *headers, size_t header_count, const void *body,
               size_t body_length);
void fetch_stop_all(void); // at exit, without output of the stopped processes
void fetch_poll(void (*done)(int id, bool ok, const char *data, size_t length));

// The text sources of js/text.js. text_width gives the width of `line` in pixels at a font size of 1 pixel.
// text_draw fills `texture` with the lines of `text` (separated by "\n"); false when no font was found.
#define TEXT_PASS_COUNT 3 // bits of `passes`, drawn in this order: fill, outline, fill
struct text_options {
    const char *text, *family; // a CSS family or generic name, such as "serif"
    bool bold, italic;
    int width, height; // of the texture
    float size; // of the font, in pixels
    float x; // of the left end, the middle or the right end of each line, as `align` says
    int align; // 0 left, 1 center, 2 right
    uint32_t fill, stroke; // 0xRRGGBBAA
    float line_width; // of the outline; its line join is always round
    int passes;
};
float text_width(const char *family, bool bold, bool italic, const char *line);
bool text_draw(unsigned texture, const struct text_options *options);

// Recording of the hydra output to a WebM file through ffmpeg (vidRecorder).
#define RECORD_FPS 25 // captureStream(25) in hydra-synth
bool recorder_start(const char *path, int width, int height);
bool recorder_active(void);
void recorder_frame(double now); // call after the hydra frame and before the code overlay
bool recorder_stop(void); // waits for ffmpeg; false when it failed
// The output frames for the patch bay (`rate` per second, rows from the bottom) into `stream`, which the publisher
// closes: the window of source_width x source_height, scaled to width x height.
bool publisher_start(int stream, int source_width, int source_height, int width, int height, int rate);
void publisher_stop(void);
// The frames of a render to file (src/render.c): each frame of `framebuffer` (width x height) goes to `stream`, the
// standard input of the ffmpeg `process`, which encoder_stop frees. A frame waits until ffmpeg took the frame before.
struct process;
void encoder_start(struct process *process, int stream, GLuint framebuffer, int width, int height);
void encoder_frame(void); // after the hydra frame
bool encoder_stop(void); // waits for ffmpeg; false when it failed
// The webRTC patch bay (src/patch_bay_main.c): one helper process with JSON lines in and out.
bool patch_bay_start(const char *program, const char *server, const char *room);
bool patch_bay_send(const char *line);
void patch_bay_poll(void (*line)(const char *text));
// Opens the output channel of the helper with `rate` frames per second, or closes it.
bool patch_bay_publish(bool on, int width, int height, int rate);
// The rate settings (frames per second): `rate` of the output, `max_rate` of each received stream.
void patch_bay_set_rates(int rate, int max_rate);
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
// Deletes a program that program_status() did not report as failed; one that still compiles goes when it is done.
void program_delete(GLuint program);
