// Video recording for vidRecorder.start() and stop(): frames of the hydra output go to ffmpeg, which writes WebM
// with VP9 like the MediaRecorder of the web editor. A writer thread keeps the pipe away from the render loop;
// when ffmpeg is slower than the frames, frames are dropped.
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

static struct {
    bool active;
    pid_t pid;
    int fd;
    int width, height;
    unsigned char *pending; // the newest frame for the writer thread
    unsigned char *writing;
    bool fresh, stopping;
    double next_frame;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t ready;
} recorder = { .lock = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER };

static void *write_frames(void *argument) {
    size_t size = (size_t)recorder.width * (size_t)recorder.height * 4;
    pthread_mutex_lock(&recorder.lock);
    for (;;) {
        while (!recorder.fresh && !recorder.stopping) pthread_cond_wait(&recorder.ready, &recorder.lock);
        if (!recorder.fresh) break;
        unsigned char *swap = recorder.writing;
        recorder.writing = recorder.pending;
        recorder.pending = swap;
        recorder.fresh = false;
        pthread_mutex_unlock(&recorder.lock);
        for (size_t done = 0; done < size;) {
            ssize_t count = write(recorder.fd, recorder.writing + done, size - done);
            if (count <= 0) break;
            done += (size_t)count;
        }
        pthread_mutex_lock(&recorder.lock);
    }
    pthread_mutex_unlock(&recorder.lock);
    return NULL;
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
    size_t bytes = (size_t)width * (size_t)height * 4;
    recorder.pid = pid;
    recorder.fd = pipe_fds[1];
    recorder.width = width;
    recorder.height = height;
    recorder.pending = malloc(bytes);
    recorder.writing = malloc(bytes);
    recorder.fresh = recorder.stopping = false;
    recorder.next_frame = 0;
    recorder.active = true;
    pthread_create(&recorder.thread, NULL, write_frames, NULL);
    return true;
}

bool recorder_active(void) { return recorder.active; }

void recorder_frame(double now) {
    if (!recorder.active || now < recorder.next_frame) return;
    recorder.next_frame = recorder.next_frame == 0 ? now + RECORD_FRAME_MS : recorder.next_frame + RECORD_FRAME_MS;
    if (recorder.next_frame < now) recorder.next_frame = now + RECORD_FRAME_MS;
    pthread_mutex_lock(&recorder.lock);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(0, 0, recorder.width, recorder.height, GL_RGBA, GL_UNSIGNED_BYTE, recorder.pending);
    recorder.fresh = true;
    pthread_cond_signal(&recorder.ready);
    pthread_mutex_unlock(&recorder.lock);
}

bool recorder_stop(void) {
    if (!recorder.active) return false;
    pthread_mutex_lock(&recorder.lock);
    recorder.stopping = true;
    pthread_cond_signal(&recorder.ready);
    pthread_mutex_unlock(&recorder.lock);
    pthread_join(recorder.thread, NULL);
    close(recorder.fd);
    int status = 0;
    waitpid(recorder.pid, &status, 0);
    free(recorder.pending);
    free(recorder.writing);
    recorder.active = false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
