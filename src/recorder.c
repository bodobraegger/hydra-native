// Video recording for vidRecorder.start() and stop(): frames of the hydra output go to ffmpeg, which writes WebM
// with VP9 like the MediaRecorder of the web editor. The same writer feeds the output to the webRTC patch bay, and
// the frames of a render to file (src/render.c) to its ffmpeg. A writer thread keeps the pipe away from the render
// loop; when the reader is slower than the frames, frames are dropped, except those of the render to file.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLES3/gl3.h>
#include <SDL3/SDL.h>

#include "native.h"
#include "process.h"

#define RGBA_BYTES 4
#define READBACK_BUFFERS 3 // one that the GPU fills, one in the pipe, and one that waits

// A frame on its way from the GPU to the pipe. The GPU copies it into a pixel buffer object (COPYING) while the
// main thread goes on; once its fence has passed, the main thread maps the buffer (QUEUED), the writer thread writes
// the mapped memory to the pipe (WRITING, then WRITTEN), and the main thread unmaps it again (FREE). So neither
// the main thread nor the writer waits for the GPU, and no thread copies the pixels.
enum readback_state { READBACK_FREE, READBACK_COPYING, READBACK_QUEUED, READBACK_WRITING, READBACK_WRITTEN };

struct readback {
    enum readback_state state;
    GLuint buffer;
    GLsync fence;          // COPYING
    const uint8_t *pixels; // QUEUED to WRITTEN: the mapped buffer
    unsigned long sequence; // the frames go to the pipe in the order of their copies
};

// Frames of the output for a pipe: ffmpeg for the recording, and the webRTC patch bay (hydra-native-patch-bay)
// while a peer receives the output.
struct frame_writer {
    bool active;
    struct process *process; // the child that reads the frames, or NULL
    int stream;
    int width, height;
    GLuint source; // the framebuffer of the frames: 0 for the window
    int source_width, source_height; // of the source, when the frames are smaller
    bool lossless; // a frame waits for a free buffer, and every frame goes to the pipe
    GLuint framebuffer, renderbuffer; // the frame at width x height, or 0 when the window has that size
    struct readback readbacks[READBACK_BUFFERS];
    unsigned long next_sequence;
    bool stopping;
    double frame_ms; // the time between two frames
    double next_frame;
    SDL_Thread *thread;
    SDL_Mutex *lock;
    SDL_Condition *changed; // a state of `readbacks` changed, or `stopping`
};

static struct frame_writer recorder = { .frame_ms = 1000.0 / RECORD_FPS }, publisher, encoder = { .lossless = true };

static size_t writer_frame_size(const struct frame_writer *writer) {
    return (size_t)writer->width * (size_t)writer->height * RGBA_BYTES;
}

// The oldest readback in `state`, or NULL.
static struct readback *oldest(struct frame_writer *writer, enum readback_state state) {
    struct readback *found = NULL;
    for (int i = 0; i < READBACK_BUFFERS; i++) {
        struct readback *readback = &writer->readbacks[i];
        if (readback->state == state && (!found || readback->sequence < found->sequence)) found = readback;
    }
    return found;
}

static int write_frames(void *argument) {
    struct frame_writer *writer = argument;
    size_t size = writer_frame_size(writer);
    SDL_LockMutex(writer->lock);
    for (;;) {
        struct readback *readback = oldest(writer, READBACK_QUEUED);
        if (!readback) {
            if (writer->stopping) break;
            SDL_WaitCondition(writer->changed, writer->lock);
            continue;
        }
        readback->state = READBACK_WRITING;
        SDL_UnlockMutex(writer->lock);
        stream_write_all(writer->stream, readback->pixels, size);
        SDL_LockMutex(writer->lock);
        readback->state = READBACK_WRITTEN;
        SDL_SignalCondition(writer->changed);
    }
    SDL_UnlockMutex(writer->lock);
    return 0;
}

static void unmap(struct readback *readback) {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, readback->buffer);
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    readback->pixels = NULL;
    readback->state = READBACK_FREE;
}

// Under the lock, on the main thread: unmaps the written frames and queues the copied ones in their order. `wait`
// waits for the oldest copy. Without `lossless`, a new frame replaces a queued one that the writer did not take.
static void collect(struct frame_writer *writer, bool wait) {
    for (int i = 0; i < READBACK_BUFFERS; i++)
        if (writer->readbacks[i].state == READBACK_WRITTEN) unmap(&writer->readbacks[i]);
    struct readback *copied;
    while ((copied = oldest(writer, READBACK_COPYING))) {
        GLenum status = glClientWaitSync(copied->fence, wait ? GL_SYNC_FLUSH_COMMANDS_BIT : 0,
                                         wait ? GL_TIMEOUT_IGNORED : 0);
        if (status == GL_TIMEOUT_EXPIRED) break;
        wait = false;
        glDeleteSync(copied->fence);
        copied->fence = NULL;
        struct readback *waiting = oldest(writer, READBACK_QUEUED);
        if (waiting && !writer->lossless) unmap(waiting);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, copied->buffer);
        copied->pixels = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)writer_frame_size(writer),
                                          GL_MAP_READ_BIT);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        copied->state = copied->pixels ? READBACK_QUEUED : READBACK_FREE;
        if (!copied->pixels) fprintf(stderr, "recorder: the driver cannot map a frame; the frame is lost\n");
        SDL_SignalCondition(writer->changed);
    }
}

// Frees what writer_start made; the thread has ended or never started.
static void free_writer(struct frame_writer *writer) {
    for (int i = 0; i < READBACK_BUFFERS; i++) {
        struct readback *readback = &writer->readbacks[i];
        if (readback->pixels) unmap(readback);
        if (readback->fence) glDeleteSync(readback->fence);
        glDeleteBuffers(1, &readback->buffer);
        *readback = (struct readback){ 0 };
    }
    SDL_DestroyCondition(writer->changed);
    SDL_DestroyMutex(writer->lock);
    writer->changed = NULL;
    writer->lock = NULL;
    glDeleteFramebuffers(1, &writer->framebuffer);
    glDeleteRenderbuffers(1, &writer->renderbuffer);
    writer->framebuffer = writer->renderbuffer = 0;
    writer->active = false;
}

// False when the writer cannot start; the caller still owns `stream` and `process` then.
static bool writer_start(struct frame_writer *writer, int stream, struct process *process, int source_width,
                         int source_height, int width, int height) {
    writer->process = process;
    writer->stream = stream;
    writer->width = width;
    writer->height = height;
    writer->source_width = source_width;
    writer->source_height = source_height;
    if (source_width != width || source_height != height) {
        glGenRenderbuffers(1, &writer->renderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, writer->renderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glGenFramebuffers(1, &writer->framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, writer->framebuffer);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, writer->renderbuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    while (glGetError() != GL_NO_ERROR) {} // the errors of earlier calls
    for (int i = 0; i < READBACK_BUFFERS; i++) {
        glGenBuffers(1, &writer->readbacks[i].buffer);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, writer->readbacks[i].buffer);
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)writer_frame_size(writer), NULL, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    writer->next_sequence = 0;
    writer->stopping = false;
    writer->next_frame = 0;
    writer->lock = SDL_CreateMutex();
    writer->changed = SDL_CreateCondition();
    bool ready = glGetError() == GL_NO_ERROR && writer->lock && writer->changed &&
                 (writer->thread = SDL_CreateThread(write_frames, "frame writer", writer));
    if (!ready) {
        fprintf(stderr, "recorder: cannot start the frame writer of %dx%d\n", width, height);
        free_writer(writer);
        return false;
    }
    writer->active = true;
    return true;
}

static void writer_frame(struct frame_writer *writer, double now) {
    if (!writer->active) return;
    SDL_LockMutex(writer->lock);
    collect(writer, false);
    if (now < writer->next_frame) {
        SDL_UnlockMutex(writer->lock);
        return;
    }
    writer->next_frame = writer->next_frame == 0 ? now + writer->frame_ms : writer->next_frame + writer->frame_ms;
    if (writer->next_frame < now) writer->next_frame = now + writer->frame_ms;
    struct readback *readback;
    while (!(readback = oldest(writer, READBACK_FREE)) && writer->lossless) {
        if (oldest(writer, READBACK_COPYING)) collect(writer, true);
        else SDL_WaitCondition(writer->changed, writer->lock);
        collect(writer, false);
    }
    if (!readback) { // the pipe is slower than the frames: this one is dropped
        SDL_UnlockMutex(writer->lock);
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, writer->source);
    // The GPU scales the window down, so that fewer bytes go to the readback and through the pipes.
    if (writer->framebuffer) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, writer->framebuffer);
        glBlitFramebuffer(0, 0, writer->source_width, writer->source_height, 0, 0, writer->width, writer->height,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, writer->framebuffer);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, readback->buffer);
    glReadPixels(0, 0, writer->width, writer->height, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    readback->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush(); // so that the fence passes also when no swap follows (a render to file, a hidden window)
    readback->state = READBACK_COPYING;
    readback->sequence = writer->next_sequence++;
    SDL_UnlockMutex(writer->lock);
}

// Ends the writer thread and closes the pipe; true when the child (if any) ended without an error. A lossless
// writer first writes all of its frames.
static bool writer_stop(struct frame_writer *writer) {
    SDL_LockMutex(writer->lock);
    while (writer->lossless && oldest(writer, READBACK_COPYING)) collect(writer, true);
    writer->stopping = true;
    SDL_SignalCondition(writer->changed);
    SDL_UnlockMutex(writer->lock);
    // A patch bay helper that reads no more must not hold the writer thread, and with it this thread, in a write.
    if (!writer->process) stream_shutdown(writer->stream);
    SDL_WaitThread(writer->thread, NULL);
    writer->thread = NULL;
    stream_close(writer->stream);
    bool ok = !writer->process || process_wait(writer->process, PROCESS_WAIT_FOREVER) == 0;
    process_free(writer->process);
    writer->process = NULL;
    free_writer(writer);
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
    if (writer_start(&recorder, input, process, width, height, width, height)) return true;
    stream_close(input);
    process_free(process);
    return false;
}

bool recorder_active(void) { return recorder.active; }

void recorder_frame(double now) {
    writer_frame(&recorder, now);
    writer_frame(&publisher, now);
}

bool recorder_stop(void) { return recorder.active && writer_stop(&recorder); }

bool publisher_start(int stream, int source_width, int source_height, int width, int height, int rate) {
    if (publisher.active) return false;
    publisher.frame_ms = 1000.0 / rate;
    return writer_start(&publisher, stream, NULL, source_width, source_height, width, height);
}

void publisher_stop(void) {
    if (publisher.active) writer_stop(&publisher);
}

bool encoder_start(struct process *process, int stream, GLuint framebuffer, int width, int height) {
    encoder.source = framebuffer;
    return writer_start(&encoder, stream, process, width, height, width, height);
}

void encoder_frame(void) { writer_frame(&encoder, 0); }

bool encoder_stop(void) { return writer_stop(&encoder); }
