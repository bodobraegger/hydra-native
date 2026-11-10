#pragma once

#include <stdbool.h>
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
