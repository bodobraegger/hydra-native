#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "native.h"
#include "process.h"

#define MAX_VIDEO_SOURCES 16

struct video_source {
    struct process *process; // NULL for a stream of the patch bay
    int stream;
    int width;
    int height;
    size_t frame_size;
    // The reader thread fills `back`, then swaps it with `front` under the lock.
    uint8_t *front;
    uint8_t *back;
    bool fresh;
    SDL_AtomicInt running;
    SDL_Thread *thread;
    SDL_Mutex *lock;
};

static struct video_source *sources[MAX_VIDEO_SOURCES];

static int read_frames(void *argument) {
    struct video_source *source = argument;
    while (SDL_GetAtomicInt(&source->running) && stream_read_all(source->stream, source->back, source->frame_size)) {
        SDL_LockMutex(source->lock);
        uint8_t *swap = source->front;
        source->front = source->back;
        source->back = swap;
        source->fresh = true;
        SDL_UnlockMutex(source->lock);
    }
    SDL_SetAtomicInt(&source->running, 0);
    return 0;
}

static int free_handle(void) {
    int handle = 0;
    while (handle < MAX_VIDEO_SOURCES && sources[handle]) handle++;
    return handle == MAX_VIDEO_SOURCES ? -1 : handle;
}

// A source that reads frames of width x height from `stream`; the source owns the stream and the process.
static int add_source(int handle, struct process *process, int stream, int width, int height) {
    struct video_source *source = calloc(1, sizeof *source);
    source->process = process;
    source->stream = stream;
    source->width = width;
    source->height = height;
    source->frame_size = (size_t)width * (size_t)height * 4;
    source->front = malloc(source->frame_size);
    source->back = malloc(source->frame_size);
    SDL_SetAtomicInt(&source->running, 1);
    source->lock = SDL_CreateMutex();
    source->thread = SDL_CreateThread(read_frames, "video", source);
    sources[handle] = source;
    return handle;
}

int video_open(const char *command, int width, int height) {
    int handle = free_handle();
    if (handle < 0 || width <= 0 || height <= 0) return -1;
    // The shell runs in a group of its own, so video_close stops the whole pipeline (helper plus gst or ffmpeg).
    int output;
    struct process *process = process_start_shell(command, NULL, &output);
    if (!process) return -1;
    return add_source(handle, process, output, width, height);
}

int video_open_stream(int stream, int width, int height) {
    int handle = free_handle();
    if (handle < 0 || width <= 0 || height <= 0) {
        stream_close(stream);
        return -1;
    }
    return add_source(handle, NULL, stream, width, height);
}

static struct video_source *find_source(int handle) {
    if (handle < 0 || handle >= MAX_VIDEO_SOURCES) return NULL;
    return sources[handle];
}

bool video_upload(int handle, GLuint texture) {
    struct video_source *source = find_source(handle);
    if (!source) return false;
    SDL_LockMutex(source->lock);
    bool fresh = source->fresh;
    if (fresh) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, source->width, source->height, GL_RGBA, GL_UNSIGNED_BYTE, source->front);
        source->fresh = false;
    }
    SDL_UnlockMutex(source->lock);
    return fresh;
}

// Copies the newest frame (RGBA) into `pixels`, which holds width * height * 4 bytes of video_size().
bool video_copy_frame(int handle, unsigned char *pixels) {
    struct video_source *source = find_source(handle);
    if (!source) return false;
    SDL_LockMutex(source->lock);
    memcpy(pixels, source->front, source->frame_size);
    SDL_UnlockMutex(source->lock);
    return true;
}

bool video_size(int handle, int *width, int *height) {
    struct video_source *source = find_source(handle);
    if (!source) return false;
    *width = source->width;
    *height = source->height;
    return true;
}

bool video_is_running(int handle) {
    struct video_source *source = find_source(handle);
    return source && SDL_GetAtomicInt(&source->running);
}

void video_close(int handle) {
    struct video_source *source = find_source(handle);
    if (!source) return;
    // The reader thread ends at the end of the stream: the processes of the group end, or the socket shuts down.
    if (source->process) {
        process_kill(source->process);
    } else {
        SDL_SetAtomicInt(&source->running, 0);
        stream_shutdown(source->stream);
    }
    SDL_WaitThread(source->thread, NULL);
    stream_close(source->stream);
    process_free(source->process);
    SDL_DestroyMutex(source->lock);
    free(source->front);
    free(source->back);
    free(source);
    sources[handle] = NULL;
}

void video_close_all(void) {
    for (int handle = 0; handle < MAX_VIDEO_SOURCES; handle++) video_close(handle);
}
