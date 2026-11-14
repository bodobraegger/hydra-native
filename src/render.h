// Render to file: the sketch frame by frame, at the rate and size of the "render" settings and as fast as the GPU
// allows, into an offscreen framebuffer, and each frame through ffmpeg into a video file. The clock of the sketch is
// the frame time, and an audio file or the sound of the video sources drives `a` and goes into the video.
#pragma once

#include <stdbool.h>

#include <GLES3/gl3.h>

#include "sandbox.h"
#include "settings.h"

// `clock_ms` is the clock of the last frame of the sketch, which the first frame of the render keeps, so that the
// time of the sketch starts at 0. The bar shows an error, such as a file name without a known extension.
void render_start(const struct settings *settings, double clock_ms);
bool render_active(void);
// Before each frame of the sketch: feeds the audio up to the frame time and sets the clock of the sketch to it.
// False when the render ended instead, because the audio file ended.
bool render_frame_begin(double *clock_ms);
// After the frame of the sketch: gives the frame to ffmpeg, shows the progress in the window, and ends the render
// after the last frame.
void render_frame_end(void);
void render_cancel(void); // deletes the partial file
// During a render, the sketch draws into this framebuffer of this size instead of the window.
GLuint render_framebuffer(void); // 0 without a render
bool render_size(int *width, int *height);
// The video source `handle` starts to play `input` of sandbox_media_input (mediaOpen), or stops (videoClose); during
// a render with renderAudio "sources", its sound goes into the mix from the frame of the call.
void render_source_play(int handle, const char *input, const struct media_playback *playback);
void render_source_stop(int handle);
