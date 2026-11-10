// Video recording for vidRecorder.start() and stop(): frames of the hydra output go to ffmpeg, which writes WebM
// with VP9 like the MediaRecorder of the web editor. The same writer feeds the output to the webRTC patch bay.
// A writer thread keeps the pipe away from the render loop; when the reader is slower than the frames, frames
// are dropped.
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <GLES3/gl3.h>

#include "native.h"

#define RECORD_FRAME_MS (1000.0 / RECORD_FPS)

// Frames of the output for a pipe: ffmpeg for the recording, and the webRTC patch bay (tools/patch-bay.mjs)
// while a peer receives the output.
struct frame_writer {
    bool active;
    pid_t pid; // the child that reads the frames, or 0
    int fd;
    int width, height;
    unsigned char *pending; // the newest frame for the writer thread
    unsigned char *writing;
    bool fresh, stopping;
    double next_frame;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t ready;
};

static struct frame_writer recorder = { .lock = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER };
static struct frame_writer publisher = { .lock = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER };

static void *write_frames(void *argument) {
    struct frame_writer *writer = argument;
    size_t size = (size_t)writer->width * (size_t)writer->height * 4;
    pthread_mutex_lock(&writer->lock);
    for (;;) {
        while (!writer->fresh && !writer->stopping) pthread_cond_wait(&writer->ready, &writer->lock);
        if (!writer->fresh) break;
        unsigned char *swap = writer->writing;
        writer->writing = writer->pending;
        writer->pending = swap;
        writer->fresh = false;
        pthread_mutex_unlock(&writer->lock);
        for (size_t done = 0; done < size;) {
            ssize_t count = write(writer->fd, writer->writing + done, size - done);
            if (count <= 0) break;
            done += (size_t)count;
        }
        pthread_mutex_lock(&writer->lock);
    }
    pthread_mutex_unlock(&writer->lock);
    return NULL;
}

static void writer_start(struct frame_writer *writer, int fd, pid_t pid, int width, int height) {
    size_t bytes = (size_t)width * (size_t)height * 4;
    writer->pid = pid;
    writer->fd = fd;
    writer->width = width;
    writer->height = height;
    writer->pending = malloc(bytes);
    writer->writing = malloc(bytes);
    writer->fresh = writer->stopping = false;
    writer->next_frame = 0;
    writer->active = true;
    pthread_create(&writer->thread, NULL, write_frames, writer);
}

static void writer_frame(struct frame_writer *writer, double now) {
    if (!writer->active || now < writer->next_frame) return;
    writer->next_frame = writer->next_frame == 0 ? now + RECORD_FRAME_MS : writer->next_frame + RECORD_FRAME_MS;
    if (writer->next_frame < now) writer->next_frame = now + RECORD_FRAME_MS;
    pthread_mutex_lock(&writer->lock);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(0, 0, writer->width, writer->height, GL_RGBA, GL_UNSIGNED_BYTE, writer->pending);
    writer->fresh = true;
    pthread_cond_signal(&writer->ready);
    pthread_mutex_unlock(&writer->lock);
}

// Ends the writer thread and closes the pipe; true when the child (if any) ended without an error.
static bool writer_stop(struct frame_writer *writer) {
    pthread_mutex_lock(&writer->lock);
    writer->stopping = true;
    pthread_cond_signal(&writer->ready);
    pthread_mutex_unlock(&writer->lock);
    pthread_join(writer->thread, NULL);
    close(writer->fd);
    int status = 0;
    if (writer->pid > 0) waitpid(writer->pid, &status, 0);
    free(writer->pending);
    free(writer->writing);
    writer->active = false;
    return writer->pid <= 0 || (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

bool recorder_start(const char *path, int width, int height) {
    if (recorder.active) return false;
    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC) != 0) return false;
    char size[32], rate[16];
    snprintf(size, sizeof size, "%dx%d", width, height);
    snprintf(rate, sizeof rate, "%d", RECORD_FPS);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipe_fds[0], STDIN_FILENO);
        execlp("ffmpeg", "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgba",
               "-s", size, "-r", rate, "-i", "-", "-vf", "vflip", "-c:v", "libvpx-vp9", "-deadline", "realtime",
               "-cpu-used", "8", "-row-mt", "1", "-b:v", "6M", path, (char *)NULL);
        _exit(127);
    }
    close(pipe_fds[0]);
    if (pid < 0) {
        close(pipe_fds[1]);
        return false;
    }
    writer_start(&recorder, pipe_fds[1], pid, width, height);
    return true;
}

bool recorder_active(void) { return recorder.active; }

void recorder_frame(double now) {
    writer_frame(&recorder, now);
    writer_frame(&publisher, now);
}

bool recorder_stop(void) { return recorder.active && writer_stop(&recorder); }

bool publisher_start(int fd, int width, int height) {
    if (publisher.active) return false;
    writer_start(&publisher, fd, 0, width, height);
    return true;
}

bool publisher_active(void) { return publisher.active; }

void publisher_stop(void) {
    if (publisher.active) writer_stop(&publisher);
}
