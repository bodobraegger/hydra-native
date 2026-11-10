#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <GLES3/gl3.h>

#define BARK_BAND_COUNT 24

// Video sources: a child process writes raw RGBA frames of a fixed size to its stdout.
int video_open(const char *command, int width, int height);
bool video_upload(int handle, GLuint texture);
bool video_is_running(int handle);
void video_close(int handle);
void video_close_all(void);

// Audio input: Meyda-compatible "loudness" features from a PulseAudio/PipeWire source.
bool audio_start(const char *device);
bool audio_features(float specific[BARK_BAND_COUNT], float *total);

// Downloads with curl; fetch_poll calls `done` once per finished download.
int fetch_start(const char *url);
void fetch_poll(void (*done)(int id, bool ok, const char *data, size_t length));

// Recording of the hydra output to a WebM file through ffmpeg (vidRecorder).
#define RECORD_FPS 25 // captureStream(25) in hydra-synth
bool recorder_start(const char *path, int width, int height);
bool recorder_active(void);
void recorder_frame(double now); // call after the hydra frame and before the code overlay
bool recorder_stop(void); // waits for ffmpeg; false when it failed
