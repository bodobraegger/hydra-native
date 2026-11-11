// Video recording for vidRecorder.start() and stop(): frames of the hydra output go to ffmpeg, which writes WebM
// with VP9 like the MediaRecorder of the web editor. The same writer feeds the output to the webRTC patch bay.
// A writer thread keeps the pipe away from the render loop; when the reader is slower than the frames, frames
// are dropped.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLES3/gl3.h>
#include <SDL3/SDL.h>

#include "native.h"
#include "process.h"

#define RECORD_FRAME_MS (1000.0 / RECORD_FPS)

// Frames of the output for a pipe: ffmpeg for the recording, and the webRTC patch bay (tools/patch-bay.mjs)
// while a peer receives the output.
struct frame_writer {
    bool active;
    struct process *process; // the child that reads the frames, or NULL
    int stream;
    int width, height;
    unsigned char *pending; // the newest frame for the writer thread
    unsigned char *writing;
    bool fresh, stopping;
    double next_frame;
    SDL_Thread *thread;
    SDL_Mutex *lock;
    SDL_Condition *ready;
};

static struct frame_writer recorder, publisher;

static int write_frames(void *argument) {
    struct frame_writer *writer = argument;
    size_t size = (size_t)writer->width * (size_t)writer->height * 4;
    SDL_LockMutex(writer->lock);
    for (;;) {
        while (!writer->fresh && !writer->stopping) SDL_WaitCondition(writer->ready, writer->lock);
        if (!writer->fresh) break;
        unsigned char *swap = writer->writing;
        writer->writing = writer->pending;
        writer->pending = swap;
        writer->fresh = false;
        SDL_UnlockMutex(writer->lock);
        stream_write_all(writer->stream, writer->writing, size);
        SDL_LockMutex(writer->lock);
    }
    SDL_UnlockMutex(writer->lock);
    return 0;
}

static void writer_start(struct frame_writer *writer, int stream, struct process *process, int width, int height) {
    size_t bytes = (size_t)width * (size_t)height * 4;
    writer->process = process;
    writer->stream = stream;
    writer->width = width;
    writer->height = height;
    writer->pending = malloc(bytes);
    writer->writing = malloc(bytes);
    writer->fresh = writer->stopping = false;
    writer->next_frame = 0;
    writer->lock = SDL_CreateMutex();
    writer->ready = SDL_CreateCondition();
    writer->active = true;
    writer->thread = SDL_CreateThread(write_frames, "frame writer", writer);
}

static void writer_frame(struct frame_writer *writer, double now) {
    if (!writer->active || now < writer->next_frame) return;
    writer->next_frame = writer->next_frame == 0 ? now + RECORD_FRAME_MS : writer->next_frame + RECORD_FRAME_MS;
    if (writer->next_frame < now) writer->next_frame = now + RECORD_FRAME_MS;
    SDL_LockMutex(writer->lock);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(0, 0, writer->width, writer->height, GL_RGBA, GL_UNSIGNED_BYTE, writer->pending);
    writer->fresh = true;
    SDL_SignalCondition(writer->ready);
    SDL_UnlockMutex(writer->lock);
}

// Ends the writer thread and closes the pipe; true when the child (if any) ended without an error.
static bool writer_stop(struct frame_writer *writer) {
    SDL_LockMutex(writer->lock);
    writer->stopping = true;
    SDL_SignalCondition(writer->ready);
    SDL_UnlockMutex(writer->lock);
    // A patch bay helper that reads no more must not hold the writer thread, and with it this thread, in a write.
    if (!writer->process) stream_shutdown(writer->stream);
    SDL_WaitThread(writer->thread, NULL);
    stream_close(writer->stream);
    bool ok = !writer->process || process_wait(writer->process, PROCESS_WAIT_FOREVER) == 0;
    process_free(writer->process);
    SDL_DestroyCondition(writer->ready);
    SDL_DestroyMutex(writer->lock);
    free(writer->pending);
    free(writer->writing);
    writer->active = false;
    return ok;
}

bool recorder_start(const char *path, int width, int height) {
    if (recorder.active) return false;
    char size[32], rate[16];
    snprintf(size, sizeof size, "%dx%d", width, height);
    snprintf(rate, sizeof rate, "%d", RECORD_FPS);
    const char *argv[] = { "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgba",
                           "-s", size, "-r", rate, "-i", "-", "-vf", "vflip", "-c:v", "libvpx-vp9", "-deadline",
                           "realtime", "-cpu-used", "8", "-row-mt", "1", "-b:v", "6M", path, NULL };
    int input;
    struct process *process = process_start(argv, &input, NULL);
    if (!process) return false;
    writer_start(&recorder, input, process, width, height);
    return true;
}

bool recorder_active(void) { return recorder.active; }

void recorder_frame(double now) {
    writer_frame(&recorder, now);
    writer_frame(&publisher, now);
}

bool recorder_stop(void) { return recorder.active && writer_stop(&recorder); }

bool publisher_start(int stream, int width, int height) {
    if (publisher.active) return false;
    writer_start(&publisher, stream, NULL, width, height);
    return true;
}

void publisher_stop(void) {
    if (publisher.active) writer_stop(&publisher);
}
